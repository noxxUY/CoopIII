// A car's bomb and a mission's mines, on every machine: the rules in
// client/src/game/mine.h with no engine, and - when a copy of the retail exe
// is handed over - every instruction the bomb and the mines rest on, read back
// out of it (client/src/game/addresses.h, "How a car bomb goes off" and "the
// mines").

#include "game/addresses.h"
#include "game/mine.h"
#include "game/missionaddr.h"
#include "game/replay.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_bombFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_bombFailures;
}

void TestTheMineRules() {
	std::printf("\na mine's rules, with no game\n");
	Check(IsMinePickup(PICKUP_MINE_INACTIVE) && IsMinePickup(PICKUP_MINE_ARMED) &&
	          IsMinePickup(PICKUP_NAUTICAL_MINE_INACTIVE) &&
	          IsMinePickup(PICKUP_NAUTICAL_MINE_ARMED),
	      "both kinds of mine, armed or not, are mines");
	Check(!IsMinePickup(PICKUP_FLOATINGPACKAGE) && !IsMinePickup(PICKUP_MONEY) &&
	          !IsMinePickup(PICKUP_NONE),
	      "a floating package, money and an empty slot are not");

	const Vec3 at{1200.0f, -600.0f, 1.0f};
	Check(SameMinePlace(at, Vec3{1201.0f, -599.0f, 2.5f}),
	      "a copy a metre off and riding a higher wave is the same mine");
	Check(!SameMinePlace(at, Vec3{1203.0f, -600.0f, 1.0f}) &&
	      !SameMinePlace(at, Vec3{1200.0f, -600.0f, 5.0f}),
	      "three metres along, or four up, is another one");

	RecentMineBlasts recent;
	Check(!recent.Had(at, 1000), "nothing has gone off yet");
	recent.Note(at, 1000);
	Check(recent.Had(Vec3{1200.5f, -600.0f, 1.0f}, 3000),
	      "the same mine said again two seconds later has been had");
	Check(!recent.Had(at, 1000 + MINE_BLAST_MEMORY_MS),
	      "and is forgotten once the memory runs out, so a new mine there can go");
	Check(!recent.Had(Vec3{0.0f, 0.0f, 0.0f}, 2000), "a mine elsewhere has not been had");
	for (uint32_t i = 0; i < MINE_BLAST_MEMORY; ++i)
		recent.Note(Vec3{float(i) * 100.0f, 0.0f, 0.0f}, 2000 + i);
	Check(!recent.Had(at, 2100) && recent.Had(Vec3{0.0f, 0.0f, 0.0f}, 2100),
	      "eight later blasts push the oldest out");

	Check(PlanMineBlast(true, true) == MineBlastHere::Already &&
	          PlanMineBlast(true, false) == MineBlastHere::Already,
	      "a blast this machine had already is nothing");
	Check(PlanMineBlast(false, true) == MineBlastHere::OurMine,
	      "one where we have the mine takes ours and goes off where it was");
	Check(PlanMineBlast(false, false) == MineBlastHere::NoMine,
	      "and one where we have none still goes off where they said");

	Check(MineBlastPlaceSane(at) && !MineBlastPlaceSane(Vec3{5000.0f, 0.0f, 0.0f}) &&
	          !MineBlastPlaceSane(Vec3{0.0f, 0.0f, 2000.0f}),
	      "a place on the map is one to say, one off it is not");

	const replay::Entry *drop     = replay::Find(scripts::op::DROP_MINE);
	const replay::Entry *nautical = replay::Find(scripts::op::DROP_NAUTICAL_MINE);
	Check(drop && nautical && drop->kind == replay::Kind::Plain && drop->count == 3 &&
	          nautical->kind == replay::Kind::Plain && nautical->count == 3 &&
	          drop->args[2] == replay::Arg::Value,
	      "both drops are replayed, three values each, to every participant");
}

// Whose detonator sets off which bomb (protocol.h, DetonatorSetsOff). Alice
// is 1 and owns the mission, bob is 2 and in it, carol is 3 and is not.
void TestWhoseDetonatorSetsOffWhat() {
	std::printf("\nwhose detonator sets off which bomb\n");
	const uint8_t in = (1u << 1) | (1u << 2);
	Check(DetonatorSetsOff(CARBOMB_REMOTE, 1, false, 1, false, INVALID_PLAYER, 0),
	      "a player's own remote bomb, mission or none");
	Check(!DetonatorSetsOff(CARBOMB_REMOTE, 1, false, 2, true, 1, in),
	      "not a bomb alice fitted for herself, whoever else presses");
	Check(DetonatorSetsOff(CARBOMB_REMOTE, 1, true, 2, true, 1, in),
	      "but her mission's, for bob, who is in her mission and holds its detonator");
	Check(!DetonatorSetsOff(CARBOMB_REMOTE, 1, true, 3, true, 1, in),
	      "and not for carol, who is not in it");
	Check(!DetonatorSetsOff(CARBOMB_REMOTE, 1, true, 2, false, 1, in),
	      "nor once the mission is over");
	Check(!DetonatorSetsOff(CARBOMB_TIMED, 1, true, 1, true, 1, in) &&
	          !DetonatorSetsOff(CARBOMB_ONIGNITIONACTIVE, 1, true, 2, true, 1, in),
	      "and a detonator sets off remote bombs only");
	Check(!DetonatorSetsOff(CARBOMB_REMOTE, INVALID_PLAYER, true, 2, true, 1, in) &&
	          !DetonatorSetsOff(CARBOMB_REMOTE, INVALID_PLAYER, false, INVALID_PLAYER, false,
	                            INVALID_PLAYER, 0),
	      "a bomb whose owner has gone is nobody's to set off");
	Check(!DetonatorSetsOff(CARBOMB_REMOTE, 2, true, 1, true, 1, in),
	      "a mission bomb somehow bob's is not the owner's to set off");

	const replay::Entry *arm = replay::Find(scripts::op::ARM_CAR_WITH_BOMB);
	Check(arm && arm->kind == replay::Kind::Plain && arm->count == 2 &&
	          arm->args[0] == replay::Arg::Car && arm->args[1] == replay::Arg::Value,
	      "ARM_CAR_WITH_BOMB is replayed to every participant, the car by its netId");
}

// ---- against the real exe ------------------------------------------------------

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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = static_cast<size_t>(va - IMAGE_BASE);
	return va >= IMAGE_BASE && o < img.size() ? img[o] : 0;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	return uint32_t(Byte(img, va)) | uint32_t(Byte(img, va + 1)) << 8 |
	       uint32_t(Byte(img, va + 2)) << 16 | uint32_t(Byte(img, va + 3)) << 24;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

// `pattern` at `va`, -1 matching any byte.
bool BytesAt(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> pattern) {
	for (int b : pattern) {
		if (b >= 0 && Byte(img, va) != static_cast<uint8_t>(b))
			return false;
		++va;
	}
	return true;
}

// Every E8 in [begin, end) that lands on `to`.
int CallsIn(const std::vector<uint8_t> &img, uint32_t begin, uint32_t end, uint32_t to) {
	int n = 0;
	for (uint32_t va = begin; va + 5 <= end; ++va)
		if (CallsTo(img, va, to))
			++n;
	return n;
}

// A dword's four bytes, for a pattern.
#define LE_DW(v)                                                                         	static_cast<int>((v) & 0xFF), static_cast<int>(((v) >> 8) & 0xFF),                  	    static_cast<int>(((v) >> 16) & 0xFF), static_cast<int>(((v) >> 24) & 0xFF)

constexpr uint32_t kTextBegin = 0x00401000;
constexpr uint32_t kTextEnd   = 0x005E4000;

void TestTheBombAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\na car's bomb against gta3.exe\n");

	// The detonator.
	Check(Dword(img, 0x00603184 + 4 * (WEAPONTYPE_DETONATOR - 2)) == 0x0055C718 &&
	          BytesAt(img, 0x0055C718, {0xDE, 0xD9, 0x55, 0xDD, 0xD8}) &&
	          CallsTo(img, CWeapon__Fire_UseDetonator, CWorld__UseDetonator),
	      "CWeapon::Fire's detonator arm hands the shooter to CWorld::UseDetonator");
	Check(CallsIn(img, kTextBegin, kTextEnd, CWorld__UseDetonator) == 1,
	      "and nothing else in the image calls it");
	Check(BytesAt(img, 0x004B4652, {0x8B, 0x35, LE_DW(CPools__ms_pVehiclePool)}) &&
	          BytesAt(img, 0x004B4671, {0x69, 0xED, 0xA8, 0x05, 0x00, 0x00}),
	      "UseDetonator walks the vehicle pool, 0x5A8 a car");
	Check(BytesAt(img, 0x004B4698, {0x83, 0xBA, LE_DW(offs::VEH_TYPE), 0x00}),
	      "automobiles only");
	Check(BytesAt(img, 0x004B46A1, {0x8A, 0x82, LE_DW(offs::AUTOMOBILE_BOMB), 0x24,
	                                offs::AUTOMOBILE_BOMB_MASK, 0x3C, CARBOMB_REMOTE}),
	      "with a remote bomb");
	Check(BytesAt(img, 0x004B46AD, {0x39, 0xBA, LE_DW(offs::AUTOMOBILE_BOMB_RIGGER)}),
	      "rigged by the shooter, compared by pointer");
	Check(BytesAt(img, 0x004B46BB, {0x24, 0xF8}) &&
	          BytesAt(img, 0x004B46C3, {0x66, 0xC7, 0x82, LE_DW(offs::VEH_BOMB_TIMER),
	                                    CARBOMB_REMOTE_FUSE_MS & 0xFF,
	                                    CARBOMB_REMOTE_FUSE_MS >> 8}),
	      "takes the bomb off and lights 500 ms");
	Check(BytesAt(img, 0x004B46CC, {0x8B, 0x82, LE_DW(offs::AUTOMOBILE_BOMB_RIGGER), 0x89,
	                                0x82, LE_DW(offs::VEH_BLOW_UP_ENTITY)}) &&
	          CallsTo(img, 0x004B46E9, CEntity__RegisterReference),
	      "and blames the rigger, registered");

	// The mission's bomb: ARM_CAR_WITH_BOMB, the 500 table's entry 78.
	Check(Dword(img, 0x005EF298 + 4 * uint32_t(scripts::op::ARM_CAR_WITH_BOMB - 500)) ==
	          0x00444459,
	      "ARM_CAR_WITH_BOMB's handler is at 0x00444459");
	Check(BytesAt(img, 0x0044447E, {0x8A, 0x9E, LE_DW(offs::AUTOMOBILE_BOMB)}) &&
	          BytesAt(img, 0x00444486, {0x80, 0xE3, 0xF8, 0x08, 0xC3}),
	      "it writes the bomb's three bits");
	Check(CallsTo(img, 0x00444491, FindPlayerPed) &&
	          BytesAt(img, 0x00444496, {0x89, 0x86, LE_DW(offs::AUTOMOBILE_BOMB_RIGGER)}) &&
	          BytesAt(img, 0x0044449C, {0x30, 0xC0}),
	      "and FindPlayerPed() as the rigger, unregistered, and nothing else: a "
	      "participant's replay names its own player");

	// The ignition.
	Check(BytesAt(img, 0x005316FA, {0x83, 0xBD, LE_DW(offs::VEH_DRIVER), 0x00}),
	      "the ignition is behind a driver in the seat");
	Check(BytesAt(img, 0x00531707, {0x8A, 0x85, LE_DW(offs::AUTOMOBILE_BOMB), 0xC0, 0xE8, 0x04,
	                                0x24, 0x01}) &&
	          offs::AUTOMOBILE_DRIVER_LAST_FRAME == 1u << 4,
	      "and only on the frame one gets in: bDriverLastFrame is bit 4");
	Check(BytesAt(img, 0x00531714, {0x8A, 0x85, LE_DW(offs::AUTOMOBILE_BOMB), 0x24, 0x07, 0x3C,
	                                CARBOMB_ONIGNITIONACTIVE}),
	      "for an armed ignition bomb");
	Check(BytesAt(img, 0x00531720, {0x66, 0xC7, 0x85, LE_DW(offs::VEH_BOMB_TIMER),
	                                CARBOMB_IGNITION_FUSE_MS & 0xFF,
	                                CARBOMB_IGNITION_FUSE_MS >> 8}) &&
	          BytesAt(img, 0x00531729, {0x8B, 0x85, LE_DW(offs::AUTOMOBILE_BOMB_RIGGER), 0x89,
	                                    0x85, LE_DW(offs::VEH_BLOW_UP_ENTITY)}) &&
	          CallsTo(img, 0x00531746, CEntity__RegisterReference),
	      "it lights a second and blames whoever the copy names as the rigger");
	Check(BytesAt(img, 0x00531769, {0x24, 0xEF, 0x0C, 0x10}),
	      "then sets bDriverLastFrame");

	// The timer, set going at the wheel.
	Check(BytesAt(img, 0x00533391, {0x8A, 0x85, LE_DW(offs::AUTOMOBILE_BOMB), 0x24, 0x07, 0x3C,
	                                CARBOMB_TIMED}) &&
	          BytesAt(img, 0x005333A3, {0x24, 0xF8, 0x0C, CARBOMB_TIMEDACTIVE}) &&
	          BytesAt(img, 0x005333AD, {0x66, 0xC7, 0x85, LE_DW(offs::VEH_BOMB_TIMER),
	                                    CARBOMB_TIMED_FUSE_MS & 0xFF,
	                                    CARBOMB_TIMED_FUSE_MS >> 8}),
	      "the timed bomb goes from 1 to 4 with seven seconds");
	Check(CallsTo(img, 0x005333B6, FindPlayerPed) &&
	          BytesAt(img, 0x005333C9, {0x89, 0x85, LE_DW(offs::VEH_BLOW_UP_ENTITY)}),
	      "blaming the player who pressed it");
	Check(BytesAt(img, 0x005333FA, {0x8A, 0x85, LE_DW(offs::AUTOMOBILE_BOMB), 0x24, 0xF8, 0x0C,
	                                CARBOMB_ONIGNITIONACTIVE}),
	      "and the ignition bomb from 2 to 5");

	// What goes off when the fuse runs out.
	Check(CallsTo(img, 0x005347E8, CVehicle__ProcessDelayedExplosion) &&
	          BytesAt(img, CVehicle__ProcessDelayedExplosion + 7,
	                  {0x66, 0x8B, 0x8D, LE_DW(offs::VEH_BOMB_TIMER)}),
	      "ProcessDelayedExplosion counts the same timer down");
	Check(BytesAt(img, 0x00551D73, {0x8B, 0x85, LE_DW(offs::VEH_BLOW_UP_ENTITY), 0x8B, 0x19,
	                                0x50, 0xFF, 0x53, 0x74}),
	      "and hands m_pBlowUpEntity to BlowUpCar through the vtable, the culprit a "
	      "replayed wreck now gets too");
	Check(Dword(img, 0x00600C1C + 4 * VTABLE_BLOW_UP_CAR) == CAutomobile__BlowUpCar,
	      "which is CAutomobile::BlowUpCar for a car");
}

void TestTheMinesAgainstTheImage(const std::vector<uint8_t> &img) {
	std::printf("\na mine against gta3.exe\n");
	const uint32_t arms[] = {0x004308B9, 0x004314A1, 0x00430A1D, 0x00430902};
	bool           table  = true;
	for (uint32_t i = 0; i < 4; ++i)
		table = table && Dword(img, CPickup__UpdateMineTable + 4 * i) == arms[i];
	Check(table, "CPickup::Update's mine table sends types 8..11 to the four arms");
	Check(BytesAt(img, 0x004314A1, {0xDE, 0xD9, 0xE9}) &&
	          0x004314A8 + Dword(img, 0x004314A4) == 0x0043094D,
	      "an armed land mine falls into the shared tail");
	Check(BytesAt(img, 0x004308ED, {0xC6, 0x06, PICKUP_MINE_ARMED}) &&
	          BytesAt(img, 0x00430AE8, {0xC6, 0x06, PICKUP_NAUTICAL_MINE_ARMED}),
	      "and each kind arms into its own type");
	Check(BytesAt(img, 0x004309E2, {0x6A, 0x00, 0x50, 0x6A, EXPLOSION_MINE, 0x6A, 0x00, 0x6A,
	                                0x00}) &&
	          CallsTo(img, CPickup__Update_MineExplosion, CExplosion__AddExplosion),
	      "the tail's explosion is AddExplosion(nil, nil, EXPLOSION_MINE, pos, 0)");
	Check(CallsIn(img, CPickup__Update, 0x004314F7, CExplosion__AddExplosion) == 1,
	      "and it is the only one CPickup::Update makes, so the redirect is all of them");
	Check(CallsTo(img, 0x004309F7, CWorld__Remove) &&
	          BytesAt(img, 0x00430A04, {0x8B, 0x19, 0x6A, 0x01, 0xFF, 0x13}) &&
	          BytesAt(img, 0x00430A0A, {0xC6, 0x46, uint8_t(offs::PICKUP_REMOVED), 0x01, 0xC7,
	                                    0x46, uint8_t(offs::PICKUP_OBJECT), 0, 0, 0, 0, 0xC6,
	                                    0x06, PICKUP_NONE}),
	      "then the object leaves the world, is deleted and the slot is freed, as "
	      "game/mine.cpp does to ours");

	const uint32_t table700 = 0x005EF4D0;
	Check(Dword(img, table700 + 4 * uint32_t(scripts::op::DROP_MINE - 700)) ==
	              COMMAND_DROP_MINE_Handler &&
	          Dword(img, table700 + 4 * uint32_t(scripts::op::DROP_NAUTICAL_MINE - 700)) ==
	              COMMAND_DROP_NAUTICAL_MINE_Handler,
	      "DROP_MINE and DROP_NAUTICAL_MINE are the 700 table's entries 52 and 53");
	Check(BytesAt(img, COMMAND_DROP_MINE_Handler, {0x8D, 0x47, 0x10, 0x89, 0xF9, 0x6A, 0x03}) &&
	          BytesAt(img, COMMAND_DROP_NAUTICAL_MINE_Handler,
	                  {0x8D, 0x47, 0x10, 0x89, 0xF9, 0x6A, 0x03}),
	      "both collect three operands");
	Check(BytesAt(img, 0x00447319, {0x6A, 0x00, 0x6A, PICKUP_MINE_INACTIVE}) &&
	          CallsTo(img, 0x00447333, CPickups__GenerateNewOne),
	      "DROP_MINE makes a PICKUP_MINE_INACTIVE");
	Check(BytesAt(img, 0x004473F2, {0x6A, 0x00, 0x6A, PICKUP_NAUTICAL_MINE_INACTIVE}) &&
	          CallsTo(img, 0x0044740C, CPickups__GenerateNewOne),
	      "and DROP_NAUTICAL_MINE a PICKUP_NAUTICAL_MINE_INACTIVE, whatever re3 says");
}

} // namespace

int RunCarBombTests() {
	TestTheMineRules();
	TestWhoseDetonatorSetsOffWhat();
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("\n  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "car bomb and the mines against one\n");
		return g_bombFailures;
	}
	std::printf("\n  reading %s\n", from.c_str());
	TestTheBombAgainstTheImage(img);
	TestTheMinesAgainstTheImage(img);
	return g_bombFailures;
}
