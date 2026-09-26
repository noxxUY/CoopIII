#include "runover.h"

#include "combat.h"
#include "leadcheck.h"
#include "ped.h"
#include "population.h"
#include "vehicle.h"
#include "../log.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <windows.h>

namespace coopiii::game {

namespace {

using FindFn     = void *(__cdecl *)();
using IsPlayerFn = bool(__thiscall *)(void *);
using InflictFn  = bool(__thiscall *)(void *, void *, uint32_t, float, uint32_t, uint32_t);

void *PlayerPed() { return Func<FindFn>(FindPlayerPed)(); }
void *PlayerVehicle() { return Func<FindFn>(FindPlayerVehicle)(); }

bool IsVehicle(const void *e) {
	return e && (Field<uint8_t>(const_cast<void *>(e), offs::ENTITY_FLAGS) & 7) ==
	                ENTITY_TYPE_VEHICLE;
}

float SpeedOf(void *entity) {
	const float *v = &Field<float>(entity, offs::MOVE_SPEED);
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Another player at the wheel of `car`, as far as this machine can tell: the
// ped in the driver's seat is one of theirs, or the session says so for a car
// whose driver isn't seated here yet.
struct OtherDriver {
	bool     yes      = false;
	uint16_t netId    = INVALID_NETID;    // when his ped is in the seat
	uint8_t  playerId = INVALID_PLAYER;   // when only the session knows
};

OtherDriver OtherDriverOf(void *car) {
	OtherDriver d;
	if (!IsVehicle(car))
		return d;
	void *const driver = Field<void *>(car, offs::VEH_DRIVER);
	if (driver && driver != PlayerPed() && RemotePlayerForPed(driver, d.netId)) {
		d.yes = true;
		return d;
	}
	d.playerId = RemoteDriverOf(car);
	d.yes      = d.playerId != INVALID_PLAYER;
	return d;
}

const char *Who(const OtherDriver &d, char *buf, size_t len) {
	if (d.netId != INVALID_NETID)
		std::snprintf(buf, len, "player net %u", d.netId);
	else
		std::snprintf(buf, len, "player %u", d.playerId);
	return buf;
}

bool g_saidDriverCounted = false;
bool g_saidHitApplied    = false;
bool g_saidHitRefused    = false;
bool g_saidOurCar        = false;
bool g_saidTheirCar      = false;

// 0x004C93AA. __fastcall for `this` in ecx; no stack arguments, like IsPlayer.
bool __fastcall DriverIsPlayer(void *driver) {
	const bool byType = Func<IsPlayerFn>(CPed__IsPlayer)(driver);
	uint16_t   netId  = INVALID_NETID;
	const bool theirs = !byType && driver && RemotePlayerForPed(driver, netId);

	if (theirs && !g_saidDriverCounted) {
		g_saidDriverCounted = true;
		void *const us  = PlayerPed();
		void *const car = us ? Field<void *>(us, object::DAMAGE_ENTITY) : nullptr;
		Log("runover: player net %u's car hit us at %.0f km/h. The engine prices it by "
		    "closing speed now, as it does for any player's car, instead of the flat "
		    "%.0f it gives a car nobody plays",
		    netId, IsVehicle(car) ? SpeedOf(car) * KMH_PER_SPEED : 0.0f,
		    CAR_HIT_FLAT_DAMAGE);
	}
	return DriverCountsAsPlayer(byType, theirs);
}

// 0x004C93ED, 0x004ECD24 and 0x004ECE91, all CPed::InflictDamage with the car
// as the culprit. `ret 14h` like the real one; the two byte-sized arguments
// arrive as whatever dword the caller pushed and go on untouched.
bool __fastcall CarHitDamage(void *ped, void * /*edx*/, void *car, uint32_t method,
                             float damage, uint32_t piece, uint32_t direction) {
	const auto engine = [&] {
		return Func<InflictFn>(CPed__InflictDamage)(ped, car, method, damage, piece,
		                                            direction);
	};
	if (!ped || !IsCarHitCause(method) || !IsVehicle(car))
		return engine();

	void *const       us     = PlayerPed();
	const OtherDriver driver = OtherDriverOf(car);

	uint16_t   victimNet    = INVALID_NETID;
	const bool victimIsUs   = ped == us;
	const bool victimPlayer = !victimIsUs && RemotePlayerForPed(ped, victimNet);
	const bool victimOther  = victimPlayer ||
	                         (!victimIsUs && AmbientReplicaForPed(ped, victimNet));
	uint16_t   hostedNet    = INVALID_NETID;
	const bool victimOurs   = !victimIsUs && !victimOther && HostedPedNetIdFor(ped, hostedNet);
	const bool carIsOurs    = us && car == PlayerVehicle() &&
	                       Field<void *>(car, offs::VEH_DRIVER) == us;

	const CarHit hit = ClassifyCarHit(true, victimIsUs, victimOther, victimOurs, carIsOurs,
	                                  driver.yes);
	char who[32];

	switch (hit) {
	case CarHit::OnUsByAnotherPlayer: {
		if (!CarHitMayLand(hit, FriendlyFireOn())) {
			if (!g_saidHitRefused) {
				g_saidHitRefused = true;
				Log("runover: %s's car hit us (cause %u, %.0f asked) and friendly fire "
				    "is off, so we are knocked down and keep our health",
				    Who(driver, who, sizeof who), method, damage);
			}
			return false;
		}
		const float health = Field<float>(ped, offs::PED_HEALTH);
		const float armour = Field<float>(ped, offs::PED_ARMOUR);
		const bool  died   = engine();
		if (!g_saidHitApplied) {
			g_saidHitApplied = true;
			Log("runover: %s's car hit us at %.0f km/h: cause %u, %.0f asked, health "
			    "%.0f -> %.0f, armour %.0f -> %.0f%s",
			    Who(driver, who, sizeof who), SpeedOf(car) * KMH_PER_SPEED, method,
			    damage, health, Field<float>(ped, offs::PED_HEALTH), armour,
			    Field<float>(ped, offs::PED_ARMOUR), died ? ", and it killed us" : "");
		}
		return died;
	}
	case CarHit::OurCarOnTheirs:
		// combat.cpp's detour refuses it next; this only says it happened.
		if (!g_saidOurCar) {
			g_saidOurCar = true;
			Log("runover: our car hit %s net %u's copy at %.0f km/h (cause %u, %.0f "
			    "asked). Its own machine decides, from its copy of our car",
			    victimPlayer ? "player" : "pedestrian", victimNet,
			    SpeedOf(car) * KMH_PER_SPEED, method, damage);
		}
		break;
	case CarHit::TheirCarOnOurs: {
		const float health = Field<float>(ped, offs::PED_HEALTH);
		const bool  died   = engine();
		if (!g_saidTheirCar) {
			g_saidTheirCar = true;
			Log("runover: %s's car hit our pedestrian net %u at %.0f km/h: cause %u, "
			    "%.0f asked, health %.0f -> %.0f%s",
			    Who(driver, who, sizeof who), hostedNet, SpeedOf(car) * KMH_PER_SPEED,
			    method, damage, health, Field<float>(ped, offs::PED_HEALTH),
			    died ? ", dead" : "");
		}
		return died;
	}
	default:
		break;
	}
	return engine();
}

// Points the call at `site` at `to`, only while it still calls `from`.
bool Redirect(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

struct Site {
	uintptr_t   at;
	uintptr_t   engine;
	uintptr_t   ours;
	const char *what;
	bool        done;
};

Site g_sites[] = {
	{CPed__ProcessControl_CarDriverIsPlayer, CPed__IsPlayer,
	 reinterpret_cast<uintptr_t>(&DriverIsPlayer), "the car hit's IsPlayer on the driver",
	 false},
	{CPed__ProcessControl_CarHitDamage, CPed__InflictDamage,
	 reinterpret_cast<uintptr_t>(&CarHitDamage), "the car hit's InflictDamage", false},
	{CPed__KillPedWithCar_KillDamage, CPed__InflictDamage,
	 reinterpret_cast<uintptr_t>(&CarHitDamage), "KillPedWithCar's kill", false},
	{CPed__KillPedWithCar_KnockDamage, CPed__InflictDamage,
	 reinterpret_cast<uintptr_t>(&CarHitDamage), "KillPedWithCar's knockdown", false},
};

} // namespace

bool InstallRunOverHooks() {
	bool ok = true;
	for (Site &s : g_sites) {
		if (s.done)
			continue;
		s.done = Redirect(s.at, s.engine, s.ours);
		if (s.done)
			Log("runover: %s at 0x%08X comes to us first", s.what,
			    static_cast<unsigned>(s.at));
		else
			Log("runover: FAILED to redirect %s at 0x%08X; it no longer calls 0x%08X",
			    s.what, static_cast<unsigned>(s.at), static_cast<unsigned>(s.engine));
		ok &= s.done;
	}
	return ok;
}

void RemoveRunOverHooks() {
	for (Site &s : g_sites) {
		if (s.done && Redirect(s.at, s.ours, s.engine))
			s.done = false;
	}
}

} // namespace coopiii::game
