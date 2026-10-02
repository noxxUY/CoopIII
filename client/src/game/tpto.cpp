#include "tpto.h"

#include "addresses.h"
#include "cheats.h"
#include "pedanim.h"
#include "../client.h"
#include "../log.h"

#include <cmath>

namespace coopiii::game {

namespace {

Client *g_client = nullptr;

using PlayerFn = void *(__cdecl *)();
using GetFn    = void *(__cdecl *)(int32_t);
using SphereFn = void *(__cdecl *)(float, float, float, float, void *, uint32_t, uint32_t,
                                   uint32_t, uint32_t, uint32_t, uint32_t);

void *Me() { return Func<PlayerFn>(FindPlayerPed)(); }

// The car our player sits in, or null.
void *MyCar(void *ped) {
	if (!ped || !Field<bool>(ped, offs::PED_IN_VEHICLE))
		return nullptr;
	return Field<void *>(ped, offs::PED_MY_VEHICLE);
}

// Where player `id` is: his ped here, or the car he sits in, when we have
// built him; otherwise his last snapshot, or where the session said he was
// when we were told about him.
bool PlaceOf(uint8_t id, Vec3 *out) {
	const RemotePlayer &p = g_client->PlayerSlot(id);
	if (p.poolHandle >= 0)
		if (void *const ped = Func<GetFn>(CPools__GetPed)(p.poolHandle)) {
			void *const  car = MyCar(ped);
			const float *at  = &Field<float>(car ? car : ped, offs::POSITION);
			*out             = Vec3{at[0], at[1], at[2]};
			return true;
		}
	if (p.haveState) {
		*out = p.last.pos;
		return true;
	}
	if (p.haveSeedPose) {
		*out = p.seedPose.pos;
		return true;
	}
	return false;
}

float GroundAt(float x, float y, float fromZ, bool &found) {
	using GroundFn = float(__cdecl *)(float, float, float, bool *);
	found          = false;
	return Func<GroundFn>(CWorld__FindGroundZFor3DCoord)(x, y, fromZ, &found);
}

bool InSight(float x0, float y0, float x1, float y1, float z) {
	struct Vec3f {
		float x, y, z;
	};
	using LineOfSightFn = bool(__cdecl *)(const Vec3f *, const Vec3f *, int, int, int, int, int,
	                                      int, int);
	const Vec3f from{x0, y0, z}, to{x1, y1, z};
	// Buildings, cars and objects; not peds, which step aside.
	return Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(&from, &to, 1, 1, 0, 1, 0, 0, 0) != 0;
}

// The car beside him on the first empty spot of the car ring (TptoCarSpot):
// ground near his, in sight of him, and nothing - building, car, ped or
// object - inside the car's bounding sphere there. False, moving nothing,
// when no spot is.
bool CarBeside(void *car, const Vec3 &at, uint8_t targetId) {
	bool        found     = false;
	const float hisGround = GroundAt(at.x, at.y, at.z + 1.5f, found);
	const float groundRef = found ? hisGround : at.z - 1.0f;
	float       radius    = Func<float(__thiscall *)(void *)>(CEntity__GetBoundRadius)(car);
	if (!(radius > 0.0f && radius < 8.0f))
		radius = 3.0f;

	for (int a = 0; a < TPTO_CAR_TRIES; ++a) {
		float dx = 0.0f, dy = 0.0f;
		TptoCarSpot(a, &dx, &dy);
		const float x = at.x + dx, y = at.y + dy;
		bool        here = false;
		const float g    = GroundAt(x, y, groundRef + 2.0f, here);
		if (!VoteSpreadGroundOk(here, g, groundRef))
			continue;
		if (!InSight(at.x, at.y, x, y, groundRef + 1.0f))
			continue;
		// The sphere sits on the ground the car will; it is the car's own,
		// which is ignored, that would otherwise fill it.
		void *const hit = Func<SphereFn>(CWorld__TestSphereAgainstWorld)(
		    x, y, g + radius * 0.5f + 0.3f, radius, car, 1, 1, 1, 1, 0, 0);
		if (hit)
			continue;

		using BaseFn   = float(__thiscall *)(void *);
		const float up = Func<BaseFn>(CEntity__GetDistanceFromCentreOfMassToBaseOfModel)(car);
		const float z  = g + up;

		// The scene round the spot first, the way LOAD_SCENE does it.
		const float scene[3] = {x, y, z};
		Func<void(__cdecl *)()>(CTimer__Stop)();
		Func<void(__cdecl *)(const float *)>(CStreaming__LoadScene)(scene);
		Func<void(__cdecl *)()>(CTimer__Update)();

		// SET_PLAYER_COORDINATES' car half: the car's own Teleport, which takes
		// it out of the world, puts it there level and still, resets its
		// suspension and adds it back.
		using TeleportFn          = void(__thiscall *)(void *, float, float, float);
		void *const *const vtable = *reinterpret_cast<void *const *const *>(car);
		reinterpret_cast<TeleportFn>(vtable[ENTITY_VT_TELEPORT / 4])(car, x, y, z);

		const char *nick = g_client->NickFor(targetId);
		Log("tpto: drove over to %s: the car is at (%.1f %.1f %.1f), try %d of the car ring",
		    nick ? nick : "?", x, y, z, a);
		return true;
	}
	return false;
}

// "Cheat activated", the game's own line, the way its handlers put it up.
void SayCheatActivated() {
	using GetTextFn = const wchar_t *(__thiscall *)(void *, const char *);
	using HelpFn    = void(__cdecl *)(const wchar_t *, bool);
	const wchar_t *text =
	    Func<GetTextFn>(CText__Get)(Ptr<void>(TheText), Ptr<char>(CHEAT_ACTIVATED_KEY));
	if (text)
		Func<HelpFn>(CHud__SetHelpMessage)(text, true);
}

TptoFacts ReadFacts(uint8_t targetId) {
	TptoFacts f;
	f.inSession = g_client->IsConnected() && g_client->LocalPlayerId() < MAX_PLAYERS;
	f.rule      = g_client->CoopCheatRule();
	f.localId   = g_client->LocalPlayerId();
	f.targetId  = targetId;
	if (targetId < MAX_PLAYERS && targetId != f.localId)
		f.targetHere = g_client->PlayerSlot(targetId).active;
	f.self   = ReadTeleportFacts();
	f.moving = MoveInProgress();

	void *const ped = Me();
	void *const car = MyCar(ped);
	f.inCar         = car != nullptr;
	if (car) {
		f.driver                  = Field<void *>(car, offs::VEH_DRIVER) == ped;
		void *const *const vtable = *reinterpret_cast<void *const *const *>(car);
		f.carMovable =
		    reinterpret_cast<uintptr_t>(vtable[ENTITY_VT_TELEPORT / 4]) == CAutomobile__Teleport;
	}

	Vec3 at{};
	if (f.targetHere && PlaceOf(targetId, &at)) {
		f.targetPlaced    = true;
		const int32_t lvl = IslandAt(at.x, at.y, at.z);
		f.otherIsland     = IslandToLoadFirst(lvl, IslandLoaded()) != 0;
		f.islandOpen      = IslandOpen(lvl, Global<int32_t>(CStats__IndustrialPassed) != 0,
		                               Global<int32_t>(CStats__CommercialPassed) != 0);
	}
	return f;
}

void Refuse(Tpto v, char digit, uint8_t targetId) {
	const char *nick = targetId < MAX_PLAYERS && targetId != g_client->LocalPlayerId()
	                       ? g_client->NickFor(targetId)
	                       : nullptr;
	char line[FEED_MESSAGE];
	TptoMessage(v, static_cast<unsigned>(digit - '0'), nick, line, sizeof line);
	Log("tpto: TPTO%c refused%s%s", digit, line[0] ? " - " : " - not in a session", line);
	g_client->Notice(line);
}

void RunTpto(char digit) {
	const uint8_t   targetId = TptoTargetOf(digit);
	const TptoFacts facts    = ReadFacts(targetId);
	Tpto            verdict  = DecideTpto(facts);
	if (verdict != Tpto::OnFoot && verdict != Tpto::InCar) {
		Refuse(verdict, digit, targetId);
		return;
	}

	Vec3 at{};
	if (!PlaceOf(targetId, &at) || !InsideWorld(at.x, at.y, at.z)) {
		Refuse(Tpto::NoPlace, digit, targetId);
		return;
	}
	const char *nick = g_client->NickFor(targetId);

	if (verdict == Tpto::InCar) {
		if (!CarBeside(MyCar(Me()), at, targetId)) {
			Refuse(Tpto::NoRoom, digit, targetId);
			return;
		}
	} else if (!MovePlayerBeside(at, targetId, 0, 1, "tpto")) {
		Refuse(Tpto::Busy, digit, targetId);
		return;
	} else {
		Log("tpto: going to %s at (%.1f %.1f %.1f)", nick ? nick : "?", at.x, at.y, at.z);
	}
	SayCheatActivated();
}

// One of COOP_CHEATS, done. A new one gets its case here.
void RunCoopCheat(const CoopCheatHit &hit) {
	switch (hit.id) {
	case COOP_CHEAT_TPTO: RunTpto(hit.arg); return;
	default:              return;
	}
}

} // namespace

void InstallTpto(Client &client) {
	g_client = &client;
	Log("tpto: TPTO1 to TPTO%u typed in play put you beside that number on the Tab list",
	    static_cast<unsigned>(MAX_PLAYERS));
}

void RemoveTpto() { g_client = nullptr; }

void TickCoopCheats() {
	CoopCheatHit  hits[4];
	const uint8_t n = DrainCoopCheats(hits, 4);
	// The key handler pushes keys typed in the pause menu too, and nothing
	// typed there is meant for the world under it.
	if (!g_client || Me() == nullptr || Global<uint8_t>(CMenuManager__m_bMenuActive) != 0) {
		if (n != 0)
			Log("tpto: %u typed cheat(s) with no player in the world or the menu up; dropped",
			    static_cast<unsigned>(n));
		return;
	}
	for (uint8_t i = 0; i < n; ++i)
		RunCoopCheat(hits[i]);
}

} // namespace coopiii::game
