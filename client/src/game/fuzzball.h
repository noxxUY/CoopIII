// The Fuzz Ball (mission 23, LUIGI5): every player picks up girls in his own
// car, and the count is the one on everybody's screen.
//
// The script, read off the retail main.scm (mission 23, offsets are from its
// start), does each of its eight girls the same way. For the first:
//
//   2454  00FD  player $528 near girl $6732 in a car, 8 x 8 x 2
//   2510  029F  player stopped
//   2522  00E0  player in any car, then 00DA $6656 = the car he is in,
//               01EA / 01E9 its seats
//   2596  01DF  SET_PLAYER_AS_LEADER girl, $528; 0164 her marker off
//   2616        round `wait 0` until 00DB girl in car $6656; 0320 (girl in the
//               player's group) false puts her marker back and gives up
//   2911        $6796 = 1: she rides
//   3022  0320  still in his group, or the marker comes back
//   3139  01A8  stopped in the cube at the police station: 01E0 clear_leader,
//               01D3 out of the car, 0211 walk to the door, $6844 += 1
//
// Every "player" there is the owner's. So a participant who stops beside a
// girl is answered for, on the owner's machine, the way the script asks it:
// 00FD true for him (he is the "subject" from there), 029F his car stopped,
// 00E0 him in a car, 00DA his car's handle (our copy of it), and 01DF makes
// his copy her leader (01DE, SET_CHAR_AS_LEADER) instead of the owner. The
// engine's own UpdateFromLeader then walks her to his car and puts her in
// it, and from there 0320 reads his copy as the player whose group she is
// in. The count, the timer and her marker are the script's own and reach
// everybody as they always did.
//
// The subject is forgotten at the next `wait` and at the next girl's first
// question (00FC), so nothing asked about "the player" outside one girl's
// pick-up is ever answered for anybody else.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coopiii::game::fuzzball {

constexpr uint16_t THE_FUZZ_BALL = 23;

constexpr uint16_t OP_WAIT                      = 0x0001;
constexpr uint16_t OP_STORE_CAR_PLAYER_IS_IN    = 0x00DA;
constexpr uint16_t OP_IS_PLAYER_IN_ANY_CAR      = 0x00E0;
constexpr uint16_t OP_LOCATE_PLAYER_ON_FOOT_CHAR_3D = 0x00FC;
constexpr uint16_t OP_LOCATE_PLAYER_IN_CAR_CHAR_3D  = 0x00FD;
constexpr uint16_t OP_SET_CHAR_AS_LEADER        = 0x01DE;
constexpr uint16_t OP_SET_PLAYER_AS_LEADER      = 0x01DF;
constexpr uint16_t OP_IS_PLAYER_STOPPED         = 0x029F;
constexpr uint16_t OP_IS_CHAR_IN_PLAYERS_GROUP  = 0x0320;

// CPed::m_leader (addresses.h, InformMyGangOfAttack's `cmp [esi+180h],eax`).
constexpr size_t PED_LEADER = 0x180;

// m_vecMoveSpeed is metres per 1/50 s: 0.02 is 3.6 km/h, a car at a stop.
constexpr float STOPPED_SPEED = 0.02f;

inline bool Stopped(float vx, float vy, float vz) {
	return vx * vx + vy * vy + vz * vz <= STOPPED_SPEED * STOPPED_SPEED;
}

// LOCATE_PLAYER_IN_CAR_CHAR_3D's box: strictly inside the three half-sizes.
inline bool InBox(float dx, float dy, float dz, float rx, float ry, float rz) {
	return std::fabs(dx) < rx && std::fabs(dy) < ry && std::fabs(dz) < rz;
}

// A participant, as the owner's machine sees him beside a girl.
struct Picker {
	float dx = 0.0f, dy = 0.0f, dz = 0.0f;   // from the girl
	bool  seated = false;                    // his copy sits in a car
	bool  valid  = false;                    // in the owner's mission, alive
};

// Who the girl is answered for: the nearest participant sitting in a car
// inside the box, or -1. A girl already following somebody is nobody's.
inline int ChoosePicker(const Picker *p, size_t n, float rx, float ry, float rz, bool girlLed) {
	if (girlLed)
		return -1;
	int   best  = -1;
	float bestD = 0.0f;
	for (size_t i = 0; i < n; ++i) {
		if (!p[i].valid || !p[i].seated || !InBox(p[i].dx, p[i].dy, p[i].dz, rx, ry, rz))
			continue;
		const float d = p[i].dx * p[i].dx + p[i].dy * p[i].dy + p[i].dz * p[i].dz;
		if (best < 0 || d < bestD) {
			best  = static_cast<int>(i);
			bestD = d;
		}
	}
	return best;
}

// How many bytes one operand takes after its type byte (addresses.h,
// SCRIPT_PARAM_*): 4 for an int32, 2 for a global or a local's number, 1 for
// an int8, 2 for an int16 and for a float in sixteenths. 0: not an operand.
inline size_t OperandBytes(uint8_t type) {
	switch (type) {
	case 1: return 4;
	case 2: return 2;
	case 3: return 2;
	case 4: return 1;
	case 5: return 2;
	case 6: return 2;
	default: return 0;
	}
}

} // namespace coopiii::game::fuzzball
