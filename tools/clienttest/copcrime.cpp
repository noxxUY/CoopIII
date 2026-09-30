// Hurting somebody else's policeman: client/src/game/copcrime.h.
//
// The swap itself, walked with no engine, and - when a copy of the retail exe
// is handed over - every table and call it rests on, read back out of it.

#include "game/copcrime.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_copFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_copFailures;
}

constexpr int32_t kCivEvents[] = {EVENT_ASSAULT, EVENT_HIT_AND_RUN, EVENT_SHOOT_PED,
                                  EVENT_PED_SET_ON_FIRE};
constexpr int32_t kCopEvents[] = {EVENT_ASSAULT_POLICE, EVENT_HIT_AND_RUN_COP,
                                  EVENT_SHOOT_COP, EVENT_COP_SET_ON_FIRE};

void TestEachPedEventHasItsPoliceTwin() {
	std::printf("\nsomebody else's policeman\n");

	bool twins = true;
	for (size_t i = 0; i < 4; ++i)
		if (CopEventFor(kCivEvents[i]) != kCopEvents[i] || !IsCivilianPedEvent(kCivEvents[i]))
			twins = false;
	Check(twins, "assault, hit and run, shoot and set on fire each have a police twin");

	bool rest = true;
	for (int32_t e = -1; e <= EVENT_LAST_REPORTED + 1; ++e) {
		bool civ = false;
		for (int32_t c : kCivEvents)
			civ = civ || c == e;
		if (!civ && (CopEventFor(e) != e || IsCivilianPedEvent(e)))
			rest = false;
	}
	Check(rest, "every other event, the police ones included, is left alone");

	bool onlyCops = true;
	for (int32_t e : kCivEvents) {
		if (EventAgainstReplica(e, PEDTYPE_COP) != CopEventFor(e))
			onlyCops = false;
		for (int type = 0; type < 23; ++type)
			if (type != PEDTYPE_COP && EventAgainstReplica(e, static_cast<uint8_t>(type)) != e)
				onlyCops = false;
	}
	Check(onlyCops, "only a host that says PEDTYPE_COP changes anything - civilians, "
	                "gangs and an old sender's 0 stay as they were");
	Check(AMBIENT_PEDTYPE_CIVMALE != PEDTYPE_COP && AMBIENT_PEDTYPE_CIVFEMALE != PEDTYPE_COP,
	      "and the type a replica is built as is never the one that swaps");
}

void TestTheSwapIsWhatSinglePlayerCharges() {
	std::printf("\nwhat the swap costs the shooter\n");

	bool dearer = true;
	for (int32_t e : kCivEvents)
		if (ChaosForCrime(CrimeForEvent(CopEventFor(e))) <= ChaosForCrime(CrimeForEvent(e)))
			dearer = false;
	Check(dearer, "every police twin is worth more chaos than the civilian one");

	Check(CrimeForEvent(EVENT_SHOOT_PED) == CRIME_SHOOT_PED &&
	          CrimeForEvent(EVENT_SHOOT_COP) == CRIME_SHOOT_COP &&
	          ChaosForCrime(CRIME_SHOOT_PED) == 30 && ChaosForCrime(CRIME_SHOOT_COP) == 80,
	      "a shot: 30 chaos as a civilian, 80 as a policeman");
	Check(StarFloorForEvent(EVENT_SHOOT_PED) == 0 && StarFloorForEvent(EVENT_SHOOT_COP) == 2,
	      "and two stars straight away for the policeman, seen or not");
	Check(CrimeForEvent(EVENT_ASSAULT) == CRIME_HIT_PED &&
	          CrimeForEvent(EVENT_ASSAULT_POLICE) == CRIME_HIT_COP &&
	          ChaosForCrime(CRIME_HIT_PED) == 5 && ChaosForCrime(CRIME_HIT_COP) == 45 &&
	          StarFloorForEvent(EVENT_ASSAULT) == 0 && StarFloorForEvent(EVENT_ASSAULT_POLICE) == 1,
	      "a punch: 5 against 45, and one star for the policeman");
	Check(CrimeForEvent(EVENT_HIT_AND_RUN) == CRIME_RUNOVER_PED &&
	          CrimeForEvent(EVENT_HIT_AND_RUN_COP) == CRIME_RUNOVER_COP &&
	          ChaosForCrime(CRIME_RUNOVER_PED) == 18 && ChaosForCrime(CRIME_RUNOVER_COP) == 80,
	      "running over: 18 against 80");
	Check(CrimeForEvent(EVENT_PED_SET_ON_FIRE) == CRIME_PED_BURNED &&
	          CrimeForEvent(EVENT_COP_SET_ON_FIRE) == CRIME_COP_BURNED &&
	          ChaosForCrime(CRIME_PED_BURNED) == 20 && ChaosForCrime(CRIME_COP_BURNED) == 80,
	      "fire: 20 against 80");
	Check(StarFloorForEvent(EVENT_HIT_AND_RUN_COP) == 0 &&
	          StarFloorForEvent(EVENT_COP_SET_ON_FIRE) == 0,
	      "running over or burning one sets no floor; the chaos does it");
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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

float Float(const std::vector<uint8_t> &img, uint32_t va) {
	const uint32_t bits = Dword(img, va);
	float          f    = 0.0f;
	std::memcpy(&f, &bits, sizeof f);
	return f;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

// Where the call an arm makes sits: `site` itself, or where the short jmp at
// `site` lands. Several arms below push their event and jump to a call they
// share with the other arm.
uint32_t CallSiteFrom(const std::vector<uint8_t> &img, uint32_t site) {
	if (Byte(img, site) == 0xEB)
		return site + 2 + static_cast<uint32_t>(static_cast<int8_t>(Byte(img, site + 1)));
	return site;
}

bool Reaches(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return CallsTo(img, CallSiteFrom(img, site), to);
}

constexpr uint32_t kTextBegin = 0x00401000;
constexpr uint32_t kTextEnd   = 0x005E4000;   // .text is 0x1E3000 long

void TestTheSwapAgainstTheImage() {
	std::printf("\nthe crime tables against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "tables against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// The hook's footing.
	Check(std::memcmp(&img[CEventList__ReportCrimeForEvent - IMAGE_BASE],
	                  REPORT_CRIME_FOR_EVENT_PROLOGUE,
	                  sizeof(REPORT_CRIME_FOR_EVENT_PROLOGUE)) == 0,
	      "ReportCrimeForEvent opens with the seven bytes the hook checks");
	Check(Dword(img, CEventList__ReportCrimeForEvent + 7) == 0x2C245C8B,
	      "and then takes the type into ebx (mov ebx,[esp+2Ch])");

	uint32_t into = 0, from5 = 0;
	for (uint32_t va = kTextBegin; va + 6 <= kTextEnd; ++va) {
		const uint8_t b = Byte(img, va);
		uint32_t      target = 0;
		if (b == 0xE8 || b == 0xE9)
			target = va + 5 + Dword(img, va + 1);
		else if (b == 0x0F && (Byte(img, va + 1) & 0xF0) == 0x80)
			target = va + 6 + Dword(img, va + 2);
		else if ((b & 0xF0) == 0x70 || b == 0xEB)
			target = va + 2 + static_cast<uint32_t>(static_cast<int8_t>(Byte(img, va + 1)));
		else
			continue;
		if (target == CEventList__ReportCrimeForEvent)
			++into;
		else if (target > CEventList__ReportCrimeForEvent &&
		         target < CEventList__ReportCrimeForEvent + sizeof(REPORT_CRIME_FOR_EVENT_PROLOGUE))
			++from5;
	}
	Check(into == 1 && CallsTo(img, REPORT_CRIME_FOR_EVENT_CALL, CEventList__ReportCrimeForEvent),
	      "one branch in the image reaches it, RegisterEvent's call at 0x00475DE8");
	Check(from5 == 0, "and none lands inside the bytes the detour overwrites");
	// RegisterEvent keeps 0x18 bytes above its arguments (four pushes and
	// `sub esp,8`), so the entity is [esp+24h]. The call pushes copsDontCare,
	// then [esp+28h] - that entity, one push later - then the type.
	const uint8_t registerPrologue[] = {0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x08};
	const uint8_t pushes[] = {0xFF, 0x74, 0x24, 0x04, 0xFF, 0x74, 0x24, 0x28,
	                          0xFF, 0x74, 0x24, 0x24};
	Check(std::memcmp(&img[CEventList__RegisterEvent - IMAGE_BASE], registerPrologue,
	                  sizeof(registerPrologue)) == 0 &&
	          std::memcmp(&img[REPORT_CRIME_FOR_EVENT_CALL - sizeof(pushes) - IMAGE_BASE],
	                      pushes, sizeof(pushes)) == 0,
	      "whose crime id is RegisterEvent's entity, the victim");

	// ReportCrimeForEvent's switch.
	bool table = true;
	for (int32_t e = 1; e <= EVENT_LAST_REPORTED; ++e) {
		const uint32_t arm   = Dword(img, REPORT_CRIME_EVENT_TABLE + 4u * uint32_t(e - 1));
		int32_t        crime = -1;
		if (arm == REPORT_CRIME_NO_CRIME_ARM)
			crime = CRIME_NONE;
		else if (Byte(img, arm) == 0xBD)   // mov ebp,imm32
			crime = static_cast<int32_t>(Dword(img, arm + 1));
		if (crime != CrimeForEvent(e)) {
			table = false;
			std::printf("    event %d files crime %d, the header says %d\n", e, crime,
			            CrimeForEvent(e));
		}
	}
	Check(table, "every event files the crime CRIME_FOR_EVENT says it does");

	// The two floors, on the type ebx carries.
	auto floorAt = [&](uint32_t cmp, uint8_t type, uint8_t level) {
		return Byte(img, cmp) == 0x83 && Byte(img, cmp + 1) == 0xFB &&
		       Byte(img, cmp + 2) == type && Byte(img, cmp + 3) == 0x75 &&
		       Byte(img, cmp + 5) == 0x6A && Byte(img, cmp + 6) == level &&
		       CallsTo(img, cmp + 7, FindPlayerPed) &&
		       CallsTo(img, cmp + 14, CPlayerPed__SetWantedLevelNoDrop);
	};
	Check(floorAt(REPORT_CRIME_FLOOR1_CMP, uint8_t(EVENT_ASSAULT_POLICE), 1) &&
	          floorAt(REPORT_CRIME_FLOOR2_CMP, uint8_t(EVENT_SHOOT_COP), 2),
	      "ASSAULT_POLICE raises the player to one star and SHOOT_COP to two");

	// Who counts as a witness: police by model, so a replica already does.
	const uint8_t modelRead[] = {0x0F, 0xBF, 0x6B, 0x5C, 0x83, 0xFD, 0x01};
	bool          noType      = true;
	for (uint32_t va = CWanted__WorkOutPolicePresence; va + 4 <= POLICE_PRESENCE_END; ++va)
		if (Dword(img, va) == offs::PED_TYPE)
			noType = false;
	Check(CallsTo(img, 0x00476117, CWanted__WorkOutPolicePresence) &&
	          std::memcmp(&img[POLICE_PRESENCE_MODEL_READ - IMAGE_BASE], modelRead,
	                      sizeof(modelRead)) == 0 &&
	          noType,
	      "police presence counts policemen by model and never reads the ped type");

	// ReportCrimeNow's chaos.
	Check(CallsTo(img, 0x004ADA3E, CWanted__ReportCrimeNow),
	      "RegisterCrime_Immediately reports through ReportCrimeNow");
	bool chaos = true;
	for (int32_t c = 1; c <= CRIME_LAST; ++c) {
		const uint32_t arm = Dword(img, REPORT_CRIME_NOW_TABLE + 4u * uint32_t(c - 1));
		float          k   = -1.0f;
		if (arm == REPORT_CRIME_NOW_TAIL)
			k = 0.0f;
		else if (Byte(img, arm) == 0xD9 && Byte(img, arm + 1) == 0x05)   // fld [k]
			k = Float(img, Dword(img, arm + 2));
		if (k != static_cast<float>(ChaosForCrime(c))) {
			chaos = false;
			std::printf("    crime %d adds %.1f, the header says %d\n", c,
			            static_cast<double>(k), ChaosForCrime(c));
		}
	}
	Check(chaos, "every crime adds the chaos CHAOS_FOR_CRIME says it does");

	// The nine places a hurt pedestrian becomes an event.
	bool sites = true;
	for (const PedEventSite &s : PED_EVENT_SITES) {
		const bool cmp = Byte(img, s.cmpAt) == 0x83 && (Byte(img, s.cmpAt + 1) & 0xF8) == 0xB8 &&
		                 Dword(img, s.cmpAt + 2) == offs::PED_TYPE &&
		                 Byte(img, s.cmpAt + 6) == PEDTYPE_COP;
		const bool pushes = Byte(img, s.copPushAt) == 0x6A && Byte(img, s.civPushAt) == 0x6A &&
		                    IsCivilianPedEvent(Byte(img, s.civPushAt + 1)) &&
		                    CopEventFor(Byte(img, s.civPushAt + 1)) == Byte(img, s.copPushAt + 1);
		const bool call = CallSiteFrom(img, s.civPushAt + 2) == s.callAt &&
		                  CallsTo(img, s.callAt, CEventList__RegisterEvent) &&
		                  Reaches(img, s.copPushAt + 2, CEventList__RegisterEvent);
		if (!cmp || !pushes || !call) {
			sites = false;
			std::printf("    %s at 0x%08X is not what the table says\n", s.what,
			            static_cast<unsigned>(s.cmpAt));
		}
	}
	Check(sites, "each of the nine tests m_nPedType == 6 and picks the police twin "
	             "CopEventFor gives");

	// And there is no tenth: every push of a civilian event that goes straight
	// into a call to RegisterEvent, or jumps to one, is an arm listed above.
	uint32_t calls = 0;
	bool     none  = true;
	for (uint32_t va = kTextBegin + 2; va + 5 <= kTextEnd; ++va) {
		if (CallsTo(img, va, CEventList__RegisterEvent))
			++calls;
		if (!Reaches(img, va, CEventList__RegisterEvent))
			continue;
		if (Byte(img, va - 2) != 0x6A || !IsCivilianPedEvent(Byte(img, va - 1)))
			continue;
		bool listed = false;
		for (const PedEventSite &s : PED_EVENT_SITES)
			listed = listed || s.civPushAt + 2 == va;
		if (!listed) {
			none = false;
			std::printf("    0x%08X registers a pedestrian event nobody listed\n",
			            static_cast<unsigned>(va));
		}
	}
	Check(calls == 18, "RegisterEvent has its eighteen callers");
	Check(none, "and every one that registers a civilian event is one of the nine");
}

} // namespace

int RunCopCrimeTests() {
	TestEachPedEventHasItsPoliceTwin();
	TestTheSwapIsWhatSinglePlayerCharges();
	TestTheSwapAgainstTheImage();
	return g_copFailures;
}
