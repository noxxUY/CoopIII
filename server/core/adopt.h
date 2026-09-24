// Who takes over a leaver's crowd (protocol.h, S_AmbientAdopt).
//
// Pure: rows in, verdicts out. Session::HandOverAmbientOf feeds it the
// leaver's peds and cars and the remaining players, and applies what comes
// back; tools/sessiontest walks it without a session at all.
//
// ## The rule
//
// Each entity goes to the remaining player nearest to it, measured in 2D,
// provided he is inside the widest distance at which GTA III itself would
// still keep that kind of entity alive around a player. Past it nobody's
// engine could keep it, so it is let go of - which is what the engine would
// have done anyway, one frame after the handover.
//
// Not the session host when nobody is near, and for the reason
// S_VehicleCustody gives about settling a car: the engine streams the world
// around its own player, so a host three streets away would be running a
// pedestrian with no collision under him, and its population code would reap
// him on its first pass. Adopting then releasing is the same outcome as
// releasing, a round trip later.
//
// The distances are the engine's widest, not its usual, and on purpose. The
// server cannot see anybody's screen, and the engine's rule is screen-
// dependent: a car behind the player goes at 50 m, one in view at 130. Hand
// over everything any engine could keep and the adopter's own reaper makes
// the on-screen call with its own camera. Too generous costs a despawn a
// frame later; too mean pops a car out of existence in front of somebody.
//
//   Traffic: CCarCtrl::PossiblyRemoveVehicle (0x00418430) takes the 2D
//   distance to FindPlayerCentreOfWorld (fsqrt of dx^2+dy^2 at 0x004184F2)
//   and keeps a car inside 50 m (0x005EC92C) off screen, 130 m (0x005EC920)
//   times GenerationDistMultiplier on screen, times 1.5 (0x005EC970) for
//   bExtendedRange. Widest: 195 m.
//
//   Pedestrians: CPopulation::ManagePopulation (0x004F3B90) takes the same 2D
//   distance (0x004F3F48) and reaps past 65 m (0x005FA894) always, 51 m
//   (0x005FA898) unless bCullExtraFarAway, 25 m (0x005FA880) off screen - each
//   times PedCreationDistMultiplier (0x004F6410), which is 1.0 on foot and
//   rises with the player's car speed to its clamp of 1.5 (0x005FA94C).
//   Widest: 97.5 m. A ped sitting in a car is skipped by that loop (the
//   bInVehicle test at 0x004F3ED0) and lives and dies with the car, so he
//   follows the car here too.
//
// ## Groups
//
// A car and whoever sits in it are decided together and go to the same
// machine, or all go. Split up, the seat breaks on every screen: a receiver
// only seats a replica ped in a replica car with the same owner, and the
// adopter can only drive a car whose driver it hosts. And the group is only
// adoptable if every member is - a police car full of cops is released whole
// rather than handed over as an empty police car.
//
// Released outright, wherever they are:
//   - a wrecked car. Nothing left to run, and the engine clears a shell a
//     minute after it burns anyway;
//   - a dead pedestrian, for the same reason;
//   - anything but a civilian (protocol.h, AmbientPedTypeAdoptable): the
//     replica is not the same kind of ped as the original, and cannot become
//     one.
#pragma once

#include "coopiii/protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coopiii {

constexpr float AMBIENT_CAR_ADOPT_RADIUS_M = 130.0f * 1.5f;
constexpr float AMBIENT_PED_ADOPT_RADIUS_M = 65.0f * 1.5f;

// A remaining player who could take something: connected, known position,
// alive. A dead player's machine is about to be moved to a hospital.
struct AdoptViewer {
	uint8_t playerId = INVALID_PLAYER;
	Vec3    pos      = {};
};

struct AdoptCar {
	uint16_t netId     = INVALID_NETID;
	Vec3     pos       = {};
	bool     destroyed = false;
};

struct AdoptPed {
	uint16_t netId        = INVALID_NETID;
	Vec3     pos          = {};
	bool     alive        = true;
	uint8_t  pedType      = 0;
	// The car his host last said he was sitting in, or INVALID_NETID.
	uint16_t vehicleNetId = INVALID_NETID;
};

// One decision. `adopter` INVALID_PLAYER means release. `group` ties a car to
// its occupants; every on-foot ped is a group of his own.
struct AdoptVerdict {
	uint16_t netId   = INVALID_NETID;
	uint8_t  kind    = AMBIENT_ADOPT_PED;
	uint8_t  adopter = INVALID_PLAYER;
	uint16_t group   = 0;
};

// Nearest viewer within `radius`, 2D, or INVALID_PLAYER. A tie goes to the
// first in the list, which the session fills in slot order.
inline uint8_t NearestAdopter(const Vec3 &at, float radius, const AdoptViewer *viewers,
                              size_t count) {
	uint8_t best  = INVALID_PLAYER;
	float   bestD = radius * radius;
	for (size_t i = 0; i < count; ++i) {
		const float dx = viewers[i].pos.x - at.x, dy = viewers[i].pos.y - at.y;
		const float d2 = dx * dx + dy * dy;
		// Inclusive, so a radius of exactly the engine's own threshold keeps
		// what the engine keeps. NaN compares false and is nobody's.
		if (d2 <= bestD && (best == INVALID_PLAYER || d2 < bestD)) {
			bestD = d2;
			best  = viewers[i].playerId;
		}
	}
	return best;
}

inline bool AdoptablePed(const AdoptPed &ped) {
	return ped.alive && AmbientPedTypeAdoptable(ped.pedType);
}

// The whole decision. Verdicts come out car by car, each car followed by its
// occupants, then the peds on foot - the order the receiver wants them in.
inline std::vector<AdoptVerdict> PlanAmbientHandover(const std::vector<AdoptCar> &cars,
                                                     const std::vector<AdoptPed> &peds,
                                                     const std::vector<AdoptViewer> &viewers) {
	std::vector<AdoptVerdict> out;
	std::vector<bool>         placed(peds.size(), false);
	uint16_t                  group = 0;

	for (const AdoptCar &car : cars) {
		bool whole = !car.destroyed;
		for (const AdoptPed &p : peds)
			if (p.vehicleNetId == car.netId && !AdoptablePed(p))
				whole = false;

		const uint8_t adopter =
		    whole ? NearestAdopter(car.pos, AMBIENT_CAR_ADOPT_RADIUS_M, viewers.data(),
		                           viewers.size())
		          : INVALID_PLAYER;

		out.push_back(AdoptVerdict{car.netId, AMBIENT_ADOPT_CAR, adopter, group});
		for (size_t i = 0; i < peds.size(); ++i) {
			if (placed[i] || peds[i].vehicleNetId != car.netId)
				continue;
			placed[i] = true;
			out.push_back(AdoptVerdict{peds[i].netId, AMBIENT_ADOPT_PED, adopter, group});
		}
		++group;
	}

	// On foot, or sitting in a car that is not the leaver's - which on every
	// screen is a ped standing where he was, since the seat pass only joins a
	// ped to a car with the same host.
	for (size_t i = 0; i < peds.size(); ++i) {
		if (placed[i])
			continue;
		const AdoptPed &p = peds[i];
		const uint8_t adopter =
		    AdoptablePed(p) ? NearestAdopter(p.pos, AMBIENT_PED_ADOPT_RADIUS_M,
		                                     viewers.data(), viewers.size())
		                    : INVALID_PLAYER;
		out.push_back(AdoptVerdict{p.netId, AMBIENT_ADOPT_PED, adopter, group++});
	}
	return out;
}

// The adopted verdicts as S_AmbientAdopt rows, a batch per packet, never
// splitting a group across two. A group bigger than a whole packet cannot
// happen - a car holds nine - and would be cut rather than dropped.
inline std::vector<std::vector<AmbientAdoptRow>> PackAdoptRows(
    const std::vector<AdoptVerdict> &verdicts) {
	std::vector<std::vector<AmbientAdoptRow>> batches;
	std::vector<AmbientAdoptRow>              group;
	uint16_t                                  groupId = 0;
	bool                                      open    = false;

	auto flush = [&]() {
		if (group.empty())
			return;
		if (batches.empty() || batches.back().size() + group.size() > MAX_ADOPT_ROWS)
			batches.emplace_back();
		for (const AmbientAdoptRow &r : group) {
			if (batches.back().size() == MAX_ADOPT_ROWS)
				batches.emplace_back();
			batches.back().push_back(r);
		}
		group.clear();
	};

	for (const AdoptVerdict &v : verdicts) {
		if (v.adopter == INVALID_PLAYER)
			continue;
		if (open && v.group != groupId)
			flush();
		groupId = v.group;
		open    = true;
		group.push_back(AmbientAdoptRow{v.netId, v.kind, v.adopter});
	}
	flush();
	return batches;
}

} // namespace coopiii
