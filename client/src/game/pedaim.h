// Aim pitch on a remote player's ped, as decisions rather than memory writes,
// so tools/clienttest can cover them without the game.
//
// The mechanism is in ped.cpp, beside the detour on
// CPedIK::PointGunInDirection (addresses.h has the proof). Short version:
// CPed::AimGun passes `push dword [5F8438h]` - a 0.0f - as the pitch for any
// ped that isn't PEDTYPE_PLAYER1..4, and every remote player is a
// CCivilianPed. The detour swaps the wire's pitch in for that argument and
// leaves everything else to the engine's own IK: which limb bends, how far,
// how fast.
//
// Pitch is in the engine's convention, the one PointGunInDirection takes:
// radians, world space, **positive is down**. CCamera::Find3rdPersonQuickAimPitch
// ends on `fchs`, so a camera looking up hands the player's ped a negative
// pitch. Both ends of the wire read and write the engine's own argument, so
// neither has to know that - but anything that invents a pitch (tools/ghost)
// does.
#pragma once

#include "pedanim.h"

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii::game {

// Straight up or straight down. The engine clamps harder than this on its own
// - the torso stops at 45 degrees and the upper arm at 70 either side of its
// parent (ms_torsoInfo 0x5F9F8C, ms_upperArmInfo 0x5F9FA4) - so this bound
// never changes what gets drawn. It's there so a corrupt packet can't hand
// MoveLimb a number that means nothing.
constexpr float AIM_PITCH_LIMIT = 1.57079632679f;

// A pitch off the wire, ready to pass to PointGunInDirection. Anything that
// isn't finite reads as level.
inline float AimPitchFromWire(float wire) {
	float pitch = 0.0f;
	if (!FiniteOr(wire, 0.0f, pitch))
		return 0.0f;
	if (pitch > AIM_PITCH_LIMIT)
		return AIM_PITCH_LIMIT;
	if (pitch < -AIM_PITCH_LIMIT)
		return -AIM_PITCH_LIMIT;
	return pitch;
}

// What the local player's own PointGunInDirection was last asked for.
//
// This is the sender's half. The ped's own fields don't answer "where is the
// player aiming" in the two cases that matter:
//
//   - m_torsoOrient.pitch, which is what used to go out, only moves for
//     weapons that aim with the torso. A pistol or an uzi (weapon.dat flag
//     128, CANAIM_WITHARM) bends the upper arm and leaves the torso at zero,
//     so the wire said "level" for the two most common guns.
//   - m_fLookDirection is 999999.0f while the player is locked on to a
//     target. CPed::SetLookFlag(CEntity*) writes that constant
//     (`mov [ebx+4BCh],497423F0h` at 0x004C64AC) and AimGun then aims through
//     PointGunAtPosition, which works the real yaw out from the target and
//     never stores it anywhere.
//
// The argument covers both, because it is the aim the local engine actually
// drew.
struct LocalAimRecord {
	const void *ped   = nullptr;
	float       yaw   = 0.0f;
	float       pitch = 0.0f;
	uint32_t    frame = 0;
	bool        valid = false;
};

// AimGun runs once a frame for an aiming ped, from CPed::ProcessControl.
// The snapshot is taken in PreFrame, before this frame's CGame::Process, so
// the newest record is normally one frame old. Two frames of slack covers a
// frame where ProcessControl skipped the ped. A record stamped in the future
// (the frame counter went backwards, which a loaded save does) is stale, and
// the unsigned subtraction is what says so.
constexpr uint32_t LOCAL_AIM_MAX_AGE_FRAMES = 2;

inline bool LocalAimFresh(const LocalAimRecord &rec, const void *ped, uint32_t now) {
	return rec.valid && ped && rec.ped == ped &&
	       now - rec.frame <= LOCAL_AIM_MAX_AGE_FRAMES;
}

// Which remote peds get their pitch swapped in this frame, and what pitch.
//
// ApplyAim fills it from PreFrame for every remote player who is aiming, and
// the detour reads it from inside the same CGame::Process. CTimer::m_FrameCounter
// only moves in CTimer::Update (`inc [009412ECh]` at 0x004AD2F1), which runs
// before CGame::Process, so both sides see the same number. An entry only
// counts for the frame it was written in: a player who stops aiming, or a ped
// freed and its pool slot reused, falls out on the next frame without anyone
// having to clear it.
//
// Keyed by pointer, not by pool handle. The pointer is only compared, never
// followed, and the frame stamp is what keeps it honest.
class ReplicaPitchTable {
public:
	void Clear() {
		for (Entry &e : m_entries)
			e = Entry{};
	}

	void Set(const void *ped, float pitch, uint32_t frame) {
		if (!ped)
			return;
		Entry *slot = nullptr;
		for (Entry &e : m_entries) {
			if (e.ped == ped) {
				slot = &e;
				break;
			}
			if (!slot && (!e.ped || e.frame != frame))
				slot = &e;
		}
		if (!slot)
			return;   // more aiming replicas this frame than players; can't happen
		slot->ped   = ped;
		slot->pitch = AimPitchFromWire(pitch);
		slot->frame = frame;
	}

	bool Find(const void *ped, uint32_t frame, float &pitch) const {
		if (!ped)
			return false;
		for (const Entry &e : m_entries) {
			if (e.ped == ped && e.frame == frame) {
				pitch = e.pitch;
				return true;
			}
		}
		return false;
	}

private:
	struct Entry {
		const void *ped   = nullptr;
		float       pitch = 0.0f;
		uint32_t    frame = 0;
	};
	Entry m_entries[MAX_PLAYERS];
};

} // namespace coopiii::game
