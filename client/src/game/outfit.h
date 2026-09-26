// A mission's change of clothes, on every participant's own player.
//
// The story dresses Claude by renaming model 0 (addresses.h, MI_PLAYER):
// UNDRESS_CHAR takes the player's ped apart and asks for the new name,
// DRESS_CHAR builds it again once the model is in. Give Me Liberty ends with
// 'playerp' going back to 'player'. Only the owner's engine runs the
// mission, so only the owner used to change; everybody else stayed in the
// prison clothes for good.
//
// Now the owner's UNDRESS_CHAR on its own player goes to everybody as a
// mission effect (game/replay.h, Kind::Outfit), and each participant makes
// the same change on its own player with its own engine's two instructions -
// when it safely can. One halfway through a door or a death has an animation
// nothing would put back, so the change waits for that to be over. One
// sitting in a car is changed where it sits: the rebuild leaves the seat
// alone and only the pose goes, and the engine's own way of seating a
// freshly built ped puts that back (addresses.h, "a seated ped built
// again"). Give Me Liberty's change comes while everybody else rides in the
// owner's car, and Luigi's scene starts before anybody gets out. It is not
// the mission's to take back afterwards: the story has moved on, and the
// clothes stay.
//
// The copies of that player on everybody else's screens follow from the
// look sync, which tells the session the moment model 0's name changes.
//
// Pure, so tools/clienttest walks it without a game. game/mission.cpp reads
// the engine and runs the instructions.
#pragma once

#include "addresses.h"
#include "look.h"

#include <coopiii/protocol.h>

#include <cstdint>
#include <cstring>

namespace coopiii::game {

// The player ped, as far as a change of clothes cares.
struct OutfitInputs {
	bool        havePed      = false;
	bool        inVehicle    = false;     // CPed::bInVehicle
	uint32_t    pedState     = 0;         // CPed::m_nPedState
	const char *model0       = nullptr;   // model 0's name now
	bool        model0Loaded = false;
	// The car it sits in, CPed::m_pMyVehicle, when there is one.
	bool        haveCar      = false;
	int32_t     carType      = 0;         // CVehicle::m_vehType
	bool        driver       = false;     // the car's pDriver is us
	bool        passenger    = false;     // one of its pPassengers is
	bool        lowCar       = false;     // CVehicle::bLowVehicle
};

enum class OutfitStep : uint8_t {
	Nothing,   // no change, or not now
	Undress,   // run UNDRESS_CHAR on our player with Look()
	Dress,     // run DRESS_CHAR on our player
};

// A state the engine's own DRESS_CHAR would leave half done: in or getting
// into or out of a seat, being dragged out of one, dying, dead or arrested.
inline bool OutfitMayChangeIn(uint32_t pedState) {
	switch (pedState) {
	case PEDSTATE_DRIVING:
	case PEDSTATE_CARJACK:
	case PEDSTATE_DRAG_FROM_CAR:
	case PEDSTATE_ENTER_CAR:
	case PEDSTATE_EXIT_CAR:
	case PEDSTATE_DIE:
	case PEDSTATE_DEAD:
	case PEDSTATE_ARRESTED:
		return false;
	default:
		return true;
	}
}

// Sitting in a car in a seat the car knows about, with nothing on the way in
// or out: what PedSetInCarCB leaves, and so what its sitting animation puts
// back after the rebuild. Not a boat, whose arm of it blends none, and not a
// train, which seats nobody through it.
inline bool OutfitSeated(const OutfitInputs &in) {
	return in.inVehicle && in.pedState == PEDSTATE_DRIVING && in.haveCar &&
	       (in.driver || in.passenger) && in.carType != VEHICLE_TYPE_BOAT &&
	       in.carType != VEHICLE_TYPE_TRAIN;
}

// The animation that seat is sat in, PedSetInCarCB's four.
inline uint16_t OutfitSitAnim(const OutfitInputs &in) {
	if (in.driver)
		return in.lowCar ? ANIM_STD_CAR_SIT_LO : ANIM_STD_CAR_SIT;
	return in.lowCar ? ANIM_STD_CAR_SIT_P_LO : ANIM_STD_CAR_SIT_P;
}

class OutfitChange {
public:
	// The owner's mission dressed its player in `look`; `model0` is what ours
	// is called now. False, and nothing to do, for a look that isn't one of
	// Claude's or one we already wear.
	bool Want(const char *look, const char *model0) {
		char clean[PLAYER_LOOK_LEN] = {};
		if (!look)
			return false;
		std::strncpy(clean, look, PLAYER_LOOK_LEN - 1);
		if (!CleanPlayerLook(clean) || !LookIsClaude(clean))
			return false;
		if (model0 && SameLook(clean, model0)) {
			m_state = State::Idle;
			return false;
		}
		std::memcpy(m_look, clean, sizeof m_look);
		std::memset(m_from, 0, sizeof m_from);
		if (model0)
			std::strncpy(m_from, model0, PLAYER_LOOK_LEN - 1);
		m_state = State::Wanted;
		return true;
	}

	OutfitStep Next(const OutfitInputs &in) {
		switch (m_state) {
		case State::Idle:
			return OutfitStep::Nothing;
		case State::Wanted:
			// Somebody else has dressed us since - this game's own script, a
			// save - or we are in it already. Either way it isn't ours to do.
			if (!in.model0 || SameLook(in.model0, m_look) || !SameLook(in.model0, m_from)) {
				m_state = State::Idle;
				return OutfitStep::Nothing;
			}
			if (!in.havePed || !in.model0Loaded)
				return OutfitStep::Nothing;
			if (in.inVehicle ? !OutfitSeated(in) : !OutfitMayChangeIn(in.pedState))
				return OutfitStep::Nothing;
			return OutfitStep::Undress;
		case State::Undressed:
			// The ped is out of the world with no body until this. Without a
			// ped there is nothing to dress, and nothing left out either.
			if (!in.havePed) {
				m_state = State::Idle;
				return OutfitStep::Nothing;
			}
			return in.model0Loaded ? OutfitStep::Dress : OutfitStep::Nothing;
		}
		return OutfitStep::Nothing;
	}

	void Undressed() { m_state = State::Undressed; }
	void Dressed() { m_state = State::Idle; }

	bool        Pending() const { return m_state != State::Idle; }
	bool        Undressing() const { return m_state == State::Undressed; }
	const char *Look() const { return m_look; }
	void        Clear() { m_state = State::Idle; }

private:
	enum class State : uint8_t { Idle, Wanted, Undressed };
	State m_state                 = State::Idle;
	char  m_look[PLAYER_LOOK_LEN] = {};
	char  m_from[PLAYER_LOOK_LEN] = {};
};

// The eight bytes UNDRESS_CHAR's label takes in the script, from a look:
// the name, zero-padded. False for a name that doesn't fit.
inline bool OutfitLabel(const char *look, uint8_t (&label)[8]) {
	std::memset(label, 0, sizeof label);
	if (!look)
		return false;
	const size_t n = std::strlen(look);
	if (n == 0 || n > sizeof label)
		return false;
	std::memcpy(label, look, n);
	return true;
}

// And back: the label an UNDRESS_CHAR carried, as a look. The engine lowers
// it itself (0x0044AB43), so this does too.
inline bool LookFromLabel(const uint8_t *label, char (&look)[PLAYER_LOOK_LEN]) {
	std::memset(look, 0, sizeof look);
	for (size_t i = 0; i < 8 && label[i] != 0; ++i)
		look[i] = static_cast<char>(label[i]);
	return CleanPlayerLook(look);
}

} // namespace coopiii::game
