#include "heligun.h"

#include "addresses.h"
#include "pedanim.h"
#include "../client.h"
#include "../hook/hook.h"
#include "../log.h"

#include <intrin.h>

namespace coopiii::game {

namespace {

Detour g_fire;   // FireOneInstantHitRound

int32_t VehicleRef(void *vehicle) {
	return Func<int32_t(__cdecl *)(void *)>(CPools__GetVehicleRef)(vehicle);
}

void *VehicleFromRef(int32_t ref) {
	if (ref < 0)
		return nullptr;
	return Func<void *(__cdecl *)(int32_t)>(CPools__GetVehicle)(ref);
}

bool IsHeli(void *entity) {
	return entity && Field<uintptr_t>(entity, offs::VTABLE) == CHeli__vtable;
}

// ---- the owner --------------------------------------------------------------

constexpr uint8_t MAX_SHOTS = 16;
OwnHeliShot g_shots[MAX_SHOTS];
uint8_t     g_shotCount = 0;

bool g_saidOtherSlot = false;

void PushShot(const OwnHeliShot &s) {
	// Oldest out first: sixteen is three seconds of two helicopters firing,
	// and HeliSync drains every frame, so this only fills with nobody in a
	// session to send to.
	if (g_shotCount == MAX_SHOTS) {
		for (uint8_t i = 1; i < MAX_SHOTS; ++i)
			g_shots[i - 1] = g_shots[i];
		--g_shotCount;
	}
	g_shots[g_shotCount++] = s;
}

void NoteOwnRound(const float *source, const float *target) {
	bool present[HELI_POLICE_SLOTS] = {};
	Vec3 pos[HELI_POLICE_SLOTS]     = {};
	void *heli[HELI_POLICE_SLOTS]   = {};
	for (int slot = 0; slot < HELI_POLICE_SLOTS; ++slot) {
		heli[slot] = Global<void *>(CHeli__pHelis + static_cast<uintptr_t>(slot) * 4);
		if (!IsHeli(heli[slot]))
			continue;
		present[slot] = true;
		pos[slot]     = ReadVec3(heli[slot], offs::POSITION);
	}

	const Vec3 src{source[0], source[1], source[2]};
	const int  slot = HeliShotSlot(src, present, pos);
	if (slot < 0) {
		if (!g_saidOtherSlot) {
			g_saidOtherSlot = true;
			Log("heli: a helicopter outside the two police slots fired; the script's "
			    "and Catalina's are not shared, so its rounds stay here");
		}
		return;
	}

	OwnHeliShot s;
	s.slot   = static_cast<uint8_t>(slot);
	s.handle = VehicleRef(heli[slot]);
	s.source = src;
	s.target = Vec3{target[0], target[1], target[2]};
	PushShot(s);
}

using FireFn = void(__cdecl *)(float *, float *, int32_t);

// Read-only. The round goes off exactly as the engine made it; this only
// notices the ones CHeli::ProcessControl fires, by the call site they come
// from - the other two callers of this function are not the helicopter's.
void __cdecl HookedFireOneInstantHitRound(float *source, float *target, int32_t damage) {
	if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == HELI_SHOT_RETURN_ADDRESS &&
	    source && target)
		NoteOwnRound(source, target);
	g_fire.Original<FireFn>()(source, target, damage);
}

uint8_t DrainOwnHeliShots(OwnHeliShot *out, uint8_t max) {
	uint8_t n = 0;
	while (n < max && n < g_shotCount) {
		out[n] = g_shots[n];
		++n;
	}
	for (uint8_t i = n; i < g_shotCount; ++i)
		g_shots[i - n] = g_shots[i];
	g_shotCount = static_cast<uint8_t>(g_shotCount - n);
	return n;
}

// ---- an observer ------------------------------------------------------------
//
// Every call below is one FireOneInstantHitRound makes, with the arguments it
// makes it with. What is left out is everything in that function that acts
// on the thing it hit: CPed::InflictDamage, CVehicle::InflictDamage, the hit
// reaction animation, the blood and CPed::Say. The six functions called here
// were walked to the bottom of their call graphs and reach none of the
// engine's damage functions (addresses.h, "the police helicopter's gun").

using AddParticleFn = void *(__cdecl *)(int32_t, const float *, const float *, void *,
                                        float, int32_t, int32_t, int32_t, int32_t);
using AddLightFn    = void(__cdecl *)(uint32_t, float, float, float, float, float, float,
                                      float, float, float, float, uint32_t, uint32_t);
using LineOfSightFn = bool(__cdecl *)(const float *, const float *, void *, void **,
                                      uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                      uint32_t, uint32_t);
using PlayOneShotFn = void(__thiscall *)(void *, int32_t, uint32_t, float);
using ScriptSoundFn = void(__cdecl *)(uint32_t, const float *);
using WaterLevelFn  = bool(__cdecl *)(float, float, float, float *, uint32_t);

void Particle(int32_t type, const float *at, const float *velocity) {
	Func<AddParticleFn>(CParticle__AddParticle)(type, at, velocity, nullptr, 0.0f, 0, 0,
	                                            0, 0);
}

void PlayOn(void *entity, uint16_t sound, float volume) {
	const int32_t id = Field<int32_t>(entity, offs::PHYSICAL_AUDIO_ENTITY);
	if (id < 0)
		return;
	Func<PlayOneShotFn>(CAudioEngine__PlayOneShot)(
	    reinterpret_cast<void *>(DMAudio_Object), id, sound, volume);
}

void ScriptSound(uint8_t sound, const float *at) {
	Func<ScriptSoundFn>(PlayOneShotScriptObject)(sound, at);
}

// The far end, 0x00563E12 onward, less CPed::Say.
void Impact(void *victim, const float *point, const float *target) {
	const float drift[3] = {0.0f, 0.0f, HELI_IMPACT_DRIFT_Z};
	const int type = victim ? (Field<uint8_t>(victim, offs::ENTITY_FLAGS) & 7) : -1;
	switch (HeliImpactFor(type)) {
	case HeliImpact::Building:
		ScriptSound(SCRIPT_SOUND_BULLET_HIT_GROUND_1, point);
		Particle(PARTICLE_SMOKE, point, drift);
		break;
	case HeliImpact::Vehicle:
		PlayOn(victim, SOUND_WEAPON_HIT_VEHICLE, 1.0f);
		break;
	case HeliImpact::Ped:
		PlayOn(victim, SOUND_WEAPON_HIT_PED, 1.0f);
		break;
	case HeliImpact::Object:
		ScriptSound(SCRIPT_SOUND_BULLET_HIT_GROUND_2, point);
		break;
	case HeliImpact::Dummy:
		ScriptSound(SCRIPT_SOUND_BULLET_HIT_GROUND_3, point);
		break;
	case HeliImpact::WaterOrNothing: {
		float level = 0.0f;
		if (!Func<WaterLevelFn>(CWaterLevel__GetWaterLevel)(
		        target[0], target[1], target[2] + HELI_SHOT_WATER_PROBE, &level, 0))
			break;
		const float splash[3] = {target[0], target[1], level};
		Particle(PARTICLE_BOAT_SPLASH, splash, drift);
		ScriptSound(SCRIPT_SOUND_BULLET_HIT_WATER, splash);
		break;
	}
	case HeliImpact::Silent:
		break;
	}
}

bool g_saidNoAudio = false;

bool DrawHeliShot(RemoteHeli &row, const Vec3 &from, const Vec3 &to) {
	void *const heli = VehicleFromRef(row.poolHandle);
	if (!heli || !IsHeli(heli))
		return false;

	// Clamped like every other wire position: both ends go into
	// CWorld::ProcessLineOfSight, which turns them into sector subscripts with
	// no bounds check (pedanim.h, ClampToWorld).
	float source[3], target[3];
	source[0] = ClampToWorld(from.x);
	source[1] = ClampToWorld(from.y);
	target[0] = ClampToWorld(to.x);
	target[1] = ClampToWorld(to.y);
	if (!FiniteOr(from.z, 0.0f, source[2]) || !FiniteOr(to.z, 0.0f, target[2]))
		return false;

	const float zero[3] = {0.0f, 0.0f, 0.0f};
	Particle(PARTICLE_GUNFLASH, source, zero);
	Func<AddLightFn>(CPointLights__AddLight)(0, source[0], source[1], source[2], 0.0f, 0.0f,
	                                         0.0f, HELI_SHOT_LIGHT_RADIUS, 1.0f, 0.8f,
	                                         0.0f, 0, 0);

	// A query and nothing more: it fills the col point and the entity and
	// stamps scan codes. What it finds decides which sound plays, not what
	// anything costs.
	alignas(16) uint8_t colPoint[64] = {};
	void *victim = nullptr;
	Func<LineOfSightFn>(CWorld__ProcessLineOfSight)(source, target, colPoint, &victim, 1, 1,
	                                                1, 1, 1, 1, 0);

	const Vec3 v = HeliTracerVelocity(Vec3{source[0], source[1], source[2]},
	                                  Vec3{target[0], target[1], target[2]});
	const float tracer[3] = {v.x, v.y, v.z};
	Particle(PARTICLE_HELI_ATTACK, source, tracer);

	Impact(victim, reinterpret_cast<const float *>(colPoint), target);

	// The report, from the helicopter, as ProcessControl plays it right after
	// the call (0x00549582).
	if (Field<int32_t>(heli, offs::PHYSICAL_AUDIO_ENTITY) >= 0)
		PlayOn(heli, SOUND_WEAPON_SHOT_FIRED, 0.0f);
	else if (!g_saidNoAudio) {
		g_saidNoAudio = true;
		Log("heli: a replica has no audio entity, so its rounds are seen and not "
		    "heard");
	}
	return true;
}

} // namespace

bool InstallHeliGunHook() {
	if (g_fire.IsInstalled())
		return true;
	if (g_fire.Install("FireOneInstantHitRound",
	                   reinterpret_cast<void *>(FireOneInstantHitRound),
	                   reinterpret_cast<void *>(&HookedFireOneInstantHitRound))) {
		Log("heli: hooked FireOneInstantHitRound at 0x%08X",
		    static_cast<unsigned>(FireOneInstantHitRound));
		return true;
	}
	Log("heli: FAILED to hook FireOneInstantHitRound at 0x%08X; our helicopter's "
	    "gunfire is not seen by anybody else",
	    static_cast<unsigned>(FireOneInstantHitRound));
	for (const auto &f : HookFailures())
		Log("heli:   %s: %s", f.name.c_str(), f.reason.c_str());
	return false;
}

void RemoveHeliGunHook() {
	if (g_fire.IsInstalled())
		g_fire.Remove();
	g_shotCount = 0;
}

void AddHeliGunToBridge(WorldBridge &bridge) {
	HeliBridge &b = bridge.heli;
	if (g_fire.IsInstalled())
		b.DrainOwnHeliShots = &DrainOwnHeliShots;
	b.DrawHeliShot = &DrawHeliShot;
}

} // namespace coopiii::game
