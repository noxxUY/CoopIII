// The lock-on, a respawn's clear, the honk, a passenger's engine and the
// everyday gangs (game/social.h), the deaths and the menu in the feed and the
// player list (chatfeed.h, boardlayout.h, game/nametag.h), and the gangs'
// threat on the replay list (game/replay.h).
//
// The pure parts run everywhere. What the engine has to agree with is checked
// against the retail gta3.exe when COOPIII_GTA3_EXE names one.

#include "boardlayout.h"
#include "chatfeed.h"
#include "game/effectshape.h"
#include "game/missionworld.h"
#include "game/nametag.h"
#include "game/replay.h"
#include "game/social.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;

namespace {

int g_socialFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_socialFailures;
}

void TestTheDecisions() {
	std::printf("\nsocial: whom the engine treats as the player\n");
	namespace g = game;
	Check(g::LockOnSkips(true, false) && !g::LockOnSkips(true, true) && !g::LockOnSkips(false, false),
	      "the lock-on passes over a teammate only with friendly fire off");
	Check(!g::RespawnClearsEffects(true, true) && g::RespawnClearsEffects(false, true) &&
	          g::RespawnClearsEffects(true, false),
	      "a respawn in a session leaves the fires and the rockets; single player and CLEAR_AREA "
	      "do not change");
	Check(g::PedalAsPad(1.0f) == 255 && g::PedalAsPad(0.5f) == 127 && g::PedalAsPad(-1.0f) == 0 &&
	          g::PedalAsPad(0.0f) == 0 && g::PedalAsPad(3.0f) == 255,
	      "a pedal reads as the pad would, 0..255, reverse as nothing");
	const uint32_t player = 1;
	Check(g::GangHatesPlayers(8, g::CHAR_CREATED_BY_RANDOM, 0x10 | player, player),
	      "a Triad of ours set on the player goes for players");
	Check(!g::GangHatesPlayers(8, g::CHAR_CREATED_BY_RANDOM, 0x10, player),
	      "one that is not, does not");
	Check(!g::GangHatesPlayers(6, g::CHAR_CREATED_BY_RANDOM, player, player) &&
	          !g::GangHatesPlayers(4, g::CHAR_CREATED_BY_RANDOM, player, player),
	      "nor does a policeman or a civilian: gangs only");
	Check(!g::GangHatesPlayers(8, 2, player, player), "nor a mission's own, which missions decide");
}

void TestTheFeedAndTheTags() {
	std::printf("\nsocial: the feed, the tag and the list\n");
	char   line[FEED_MESSAGE];
	size_t nickLen = 0;
	FormatWasted(line, sizeof line, "alice", "bob", &nickLen);
	Check(std::strcmp(line, "alice was killed by bob") == 0 && nickLen == 5, "killed by");
	FormatWasted(line, sizeof line, "alice", nullptr, &nickLen);
	Check(std::strcmp(line, "alice was wasted") == 0, "wasted");
	FormatWasted(line, sizeof line, "alice", "alice", &nickLen);
	Check(std::strcmp(line, "alice was wasted") == 0, "a player is never his own killer");
	FormatBusted(line, sizeof line, "~x", &nickLen);
	Check(std::strcmp(line, "-x was busted") == 0 && nickLen == 2, "busted, and a name drawn safe");
	Check(BustedJustNow(true, 1, 56, 56) && !BustedJustNow(true, 56, 56, 56) &&
	          !BustedJustNow(false, 0, 56, 56),
	      "an arrest is news once, and not in the first snapshot of somebody");

	char hp[16] = "* 100";
	game::TagPaused(hp, sizeof hp);
	Check(std::strcmp(hp, "* 100 PAUSED") == 0, "the tag says paused after the health");
	char tight[8] = "WASTED";
	game::TagPaused(tight, sizeof tight);
	Check(std::strlen(tight) < sizeof tight, "and never runs past its buffer");

	Check(StateOf(false, 80.0f, false, 0, 0, true) == BoardState::Paused &&
	          StateOf(false, 80.0f, false, 0, 99999, true) == BoardState::Away &&
	          StateOf(false, 80.0f, false, 0, 0) == BoardState::OnFoot,
	      "the list says paused, and a player gone quiet is still away");
	char label[24];
	StateLabel(label, sizeof label, BoardState::Paused);
	Check(std::strcmp(label, "PAUSED") == 0, "in those words");
}

void TestTheGangsAreReplayed() {
	std::printf("\nsocial: a gang set on the player is set on everybody\n");
	using namespace game::replay;
	const Entry *on  = Find(0x03F1);
	const Entry *off = Find(0x03F2);
	Check(on && off && on->kind == Kind::World && off->kind == Kind::World && on->count == 2 &&
	          off->count == 2 && on->args[0] == Arg::Value && on->args[1] == Arg::Value,
	      "SET_ and CLEAR_THREAT_FOR_PED_TYPE are world instructions of two values");
	const game::shape::RepeatRule a = game::shape::RuleOf(0x03F1), b = game::shape::RuleOf(0x03F2);
	Check(a.rule == game::shape::Repeat::State && b.rule == game::shape::Repeat::State && a.family == b.family &&
	          a.identity == 2,
	      "and on and off are one setting per type and threat");
}

// ---- against the image -------------------------------------------------------------------

bool LoadImage(std::vector<uint8_t> &image, std::string &from) {
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
		if (got == image.size() && image.size() == game::IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint8_t At(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = static_cast<size_t>(va - game::IMAGE_BASE);
	return va >= game::IMAGE_BASE && o < img.size() ? img[o] : 0;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	return static_cast<uint32_t>(At(img, va)) | (static_cast<uint32_t>(At(img, va + 1)) << 8) |
	       (static_cast<uint32_t>(At(img, va + 2)) << 16) | (static_cast<uint32_t>(At(img, va + 3)) << 24);
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> pattern) {
	for (int b : pattern) {
		if (b >= 0 && At(img, va) != static_cast<uint8_t>(b))
			return false;
		++va;
	}
	return true;
}

bool Within(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes, std::initializer_list<int> pattern) {
	for (uint32_t i = 0; i < bytes; ++i)
		if (Bytes(img, fn + i, pattern))
			return true;
	return false;
}

uint32_t CallAt(const std::vector<uint8_t> &img, uint32_t va) {
	return At(img, va) == 0xE8 ? va + 5 + Dword(img, va + 1) : 0;
}

float Float(const std::vector<uint8_t> &img, uint32_t va) {
	const uint32_t v = Dword(img, va);
	float          f = 0.0f;
	std::memcpy(&f, &v, sizeof f);
	return f;
}

#define LE32(v)                                                                          \
	static_cast<int>((v) & 0xFF), static_cast<int>(((v) >> 8) & 0xFF),                  \
	    static_cast<int>(((v) >> 16) & 0xFF), static_cast<int>(((v) >> 24) & 0xFF)

void TestAgainstTheImage() {
	std::printf("\nsocial: against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadImage(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check them against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());
	namespace g = game;

	// The lock-on.
	Check(CallAt(img, 0x004F21B7) == g::CPlayerPed__FindWeaponLockOnTarget &&
	          CallAt(img, 0x004F224F) == g::CPlayerPed__FindNextWeaponLockOnTarget &&
	          CallAt(img, 0x004F226A) == g::CPlayerPed__FindNextWeaponLockOnTarget,
	      "the two lock-on searches are where CPlayerPed calls them");
	Check(Bytes(img, g::LockOn_CanSeeCall - 3, {0x89, 0xD9, 0x56}) &&
	          CallAt(img, g::LockOn_CanSeeCall) == g::CPed__OurPedCanSeeThisOne &&
	          Bytes(img, g::NextLockOn_CanSeeCall - 3, {0x89, 0xF9, 0x55}) &&
	          CallAt(img, g::NextLockOn_CanSeeCall) == g::CPed__OurPedCanSeeThisOne,
	      "each asks OurPedCanSeeThisOne about the ped, the player in ecx");
	Check(Within(img, 0x004F29E4, 0x20, {0x83, 0xF8, 0x30}) &&
	          Within(img, 0x004F29E4, 0x20, {0x80, 0xBE, 0x14, 0x03, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x004F2A02, {0x39, 0x86, 0x80, 0x01, 0x00, 0x00}),
	      "after skipping the dead, the seated and the player's followers - and nobody else");
	Check(Bytes(img, 0x004C57D1, {0xC2, 0x04, 0x00}), "the sight test takes one ped and pops it");

	// The respawn's clear.
	const uint32_t sites[] = {g::GameLogic_WastedClearCall, g::GameLogic_BustedClearCall,
	                          g::GameLogic_FailedClearCall};
	bool all = true;
	for (uint32_t s : sites)
		all = all && CallAt(img, s) == g::CWorld__ClearExcitingStuffFromArea &&
		      Bytes(img, s - 0x13, {0x6A, 0x01, 0xFF, 0x35, 0x70, 0xCD, 0x5E, 0x00});
	Check(all && Float(img, 0x005ECD70) == 4000.0f,
	      "CGameLogic::Update clears 4000 m around the player, projectiles too, in each of its arms");
	Check(Bytes(img, 0x004B5060, {0xB9, LE32(g::gFireManager)}) &&
	          CallAt(img, g::ClearExciting_FireCall) == g::CFireManager__ExtinguishPoint &&
	          Within(img, g::CFireManager__ExtinguishPoint, 0x100, {0xC2, 0x10, 0x00}) &&
	          CallAt(img, g::ClearExciting_CarFireCall) == g::CWorld__ExtinguishAllCarFiresInArea &&
	          Bytes(img, g::ClearExciting_CarFireCall + 5, {0x83, 0xC4, 0x10}) &&
	          CallAt(img, g::ClearExciting_ExplosionCall) == g::CExplosion__RemoveAllExplosionsInArea &&
	          Bytes(img, g::ClearExciting_ExplosionCall + 5, {0x83, 0xC4, 0x10}) &&
	          Bytes(img, 0x004B509E, {0x80, 0x7C, 0x24, 0x3C, 0x00, 0x74, 0x0A}) &&
	          CallAt(img, g::ClearExciting_ProjectileCall) == g::CProjectileInfo__RemoveAllProjectiles,
	      "then puts out the fires, the burning cars and the explosions, and with the flag the "
	      "projectiles");
	Check(CallAt(img, 0x004B54F7) == 0x00552AF0 && Float(img, 0x006025A0) == 300.0f &&
	          Bytes(img, 0x00552B4F, {0xC7, 0x85, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
	      "a burning car is raised to 300 and its fire timer zeroed");
	Check(CallAt(img, 0x0044D8DD) == g::CWorld__ClearExcitingStuffFromArea,
	      "CLEAR_AREA calls it from elsewhere, and keeps doing all of it");

	// The honk.
	bool test = true;
	for (size_t i = 0; i < sizeof g::SLOWCARDOWN_STATUS_TEST; ++i)
		test = test && At(img, g::SlowCarDown_StatusTest + static_cast<uint32_t>(i)) ==
		                   g::SLOWCARDOWN_STATUS_TEST[i];
	Check(test && g::SlowCarDown_StatusTest + sizeof g::SLOWCARDOWN_STATUS_TEST ==
	                  g::SlowCarDown_PlayerCar &&
	          Bytes(img, g::SlowCarDown_PlayerCar, {0x83, 0xFE, 0x09}) &&
	          Bytes(img, g::SlowCarDown_PlayerCar + 9, {0x80, 0xBB, 0x60, 0x01, 0x00, 0x00, 0x01}),
	      "the car scan tests STATUS_PLAYER inline, then the flee state and a random ped");
	Check(Bytes(img, 0x0041958A, {0x0F, 0x85, 0xE5, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x0041974E, {0x80, 0xBD, 0x2C, 0x02, 0x00, 0x00, 0x00}),
	      "is branched to only at its start, and reads the horn after it");

	// The engine's gas.
	Check(CallAt(img, 0x0056A6B0) == g::FindPlayerVehicle && CallAt(img, 0x0056A6C9) == 0x0056B0D0,
	      "the player's car's engine is played off the pad");
	bool pads = true;
	for (uint32_t s : g::ENGINE_ACCELERATE_CALLS)
		pads = pads && CallAt(img, s) == g::CPad__GetAccelerate &&
		       Within(img, s - 7, 7, {0xB9, LE32(g::CPad__Pads)});
	for (uint32_t s : g::ENGINE_BRAKE_CALLS)
		pads = pads && CallAt(img, s) == g::CPad__GetBrake &&
		       Within(img, s - 7, 7, {0xB9, LE32(g::CPad__Pads)});
	Check(pads, "every one of those reads Pads[0]'s gas or brake");
	Check(Bytes(img, g::CPad__GetAccelerate, {0x80, 0xB9, 0xDF, 0x00, 0x00, 0x00, 0x00}) &&
	          Bytes(img, g::CPad__GetAccelerate + 9, {0x31, 0xC0, 0xC3}),
	      "which takes nothing but the pad and returns in eax");

	// The gangs' threat.
	const uint32_t t1000 = g::g_ScriptOpcodeTable_1000;
	const uint32_t on = Dword(img, t1000 + 4 * (0x03F1 - 1001));
	const uint32_t off = Dword(img, t1000 + 4 * (0x03F2 - 1001));
	Check(on == 0x00588677 && off == 0x005886A7 && Bytes(img, on + 5, {0x6A, 0x02}) &&
	          Bytes(img, on + 0x12, {0x8B, 0x2C, 0x85, LE32(g::CPedType__ms_apPedType)}) &&
	          Bytes(img, on + 0x19, {0x8B, 0x45, static_cast<int>(g::offs::PEDTYPE_THREATS)}) &&
	          Bytes(img, off + 0x21, {0xF7, 0xD0, 0x21, 0xC3}),
	      "03F1 ORs a threat into a type's m_threats, 03F2 takes it out, two operands each");
}
#undef LE32

} // namespace

int RunSocialTests() {
	TestTheDecisions();
	TestTheFeedAndTheTags();
	TestTheGangsAreReplayed();
	TestAgainstTheImage();
	return g_socialFailures;
}
