// A local player who can no longer get into any car, noticed and undone.
//
// Reported from an internet session: the enter key walked the player up to
// the car and he never got in, and after that the same happened at every car.
// The engine's own entry has one gate that belongs to the ped rather than to
// the car: CPed::SetEnterCar returns having done nothing while m_pVehicleAnim
// is set (`cmp dword [ebx+1D8h],0 / jne` at 0x004E0A07), and SetCarJack has
// the same test. SeekCar then stands the ped at the door with the objective
// still on, and the next car does the same. Every way out of a car the engine
// has nils that pointer; a way out that does not leaves the player on foot
// holding his sitting animation, and every door refuses him from then on.
// game/seat.cpp's seat key was one such way out (UnseatLocalPlayer).
//
// The holes are fixed where they were. This is what catches the next one:
// the local player's engine read once a tick (WorldBridge::ProbeLocalEntry),
// and the three states no ordinary entry is ever in for long.
//
// Pure, so tools/clienttest drives every answer without a game.
#pragma once

#include <coopiii/protocol.h>

#include <cstdint>

namespace coopiii {

// One tick's reading of the local player's ped, as far as getting into a car
// goes. Filled in by game/seat.cpp.
struct LocalEntryProbe {
	bool     havePed        = false;
	bool     inVehicle      = false;   // bInVehicle, with a car behind it
	bool     seatedState    = false;   // PED_DRIVING
	bool     inControlState = false;   // m_nPedState <= PED_STATES_NO_AI
	bool     enterObjective = false;   // OBJECTIVE_ENTER_CAR_AS_DRIVER or _AS_PASSENGER
	bool     seeking        = false;   // PED_SEEK_CAR: walking to the door
	bool     entering       = false;   // PED_ENTER_CAR or PED_CARJACK
	bool     exiting        = false;   // PED_EXIT_CAR or PED_DRAG_FROM_CAR
	bool     vehicleAnim    = false;   // m_pVehicleAnim is not null
	bool     controlsOn     = false;   // CPad holds no lock on the player but CoopIII's own
	uint32_t animMark       = 0;       // changes while an entry's animation plays
	float    x = 0.0f, y = 0.0f;       // where the ped stands
};

enum class EntryStuck : uint8_t {
	None,
	// On foot, in control, and still holding a car's animation. The one
	// that locked the player out of every car.
	StaleVehicleAnim,
	// PED_DRIVING with no car under him: a car taken away while he sat in it,
	// or a warp that set the state and then gave him no seat.
	SeatedOnFoot,
	// Walking to a door or opening one, and nothing has moved for
	// ENTRY_STUCK_MS: the engine is refusing him the door.
	NoProgress,
};

// How long each has to last before it is undone. The stale animation is
// never legitimate, so it only has to outlast the frame that sets it; the
// seated state on foot is given a moment for a callback to finish; the stall
// is longer than the slowest door and the slowest jack.
constexpr uint32_t ENTRY_STALE_ANIM_MS    = 300;
constexpr uint32_t ENTRY_SEATED_ONFOOT_MS = 1500;
constexpr uint32_t ENTRY_STUCK_MS         = 5000;
// Standing this far from where the stall began counts as progress.
constexpr float    ENTRY_MOVED_M          = 0.5f;
// A gap between two looks longer than this is a pause (the menu, a load), and
// the time in it does not count towards anything.
constexpr uint32_t ENTRY_WATCH_GAP_MS     = 1000;

// The session holds a seat for us that our own engine says we are not in:
// on foot, not getting in or out, for this long.
constexpr uint32_t SESSION_SEAT_STALE_MS  = 4000;

class EntryStuckWatch {
public:
	// `ownEntry` is an entry CoopIII is driving by itself - the seat key's,
	// or a ride a script of ours started - which has a deadline of its own
	// and is left to it. Returns what to undo, once; the timer that fired
	// starts again from zero.
	EntryStuck Tick(const LocalEntryProbe &p, bool ownEntry, uint32_t nowMs) {
		if (!p.havePed) {
			Reset();
			return EntryStuck::None;
		}
		if (!m_polled) {
			m_staleSinceMs  = nowMs;
			m_seatedSinceMs = nowMs;
			Anchor(p, nowMs);
		} else if (nowMs - m_lastMs > ENTRY_WATCH_GAP_MS) {
			const uint32_t gap = nowMs - m_lastMs;
			m_staleSinceMs += gap;
			m_seatedSinceMs += gap;
			m_movedMs += gap;
		}
		m_polled = true;
		m_lastMs = nowMs;

		// 1. The car animation still hanging off a ped on his feet.
		const bool stale = !p.inVehicle && p.inControlState && p.vehicleAnim;
		if (!stale)
			m_staleSinceMs = nowMs;
		else if (nowMs - m_staleSinceMs >= ENTRY_STALE_ANIM_MS) {
			m_staleSinceMs = nowMs;
			return EntryStuck::StaleVehicleAnim;
		}

		// 2. Sitting, in no car.
		const bool seatedOnFoot = !p.inVehicle && p.seatedState;
		if (!seatedOnFoot)
			m_seatedSinceMs = nowMs;
		else if (nowMs - m_seatedSinceMs >= ENTRY_SEATED_ONFOOT_MS) {
			m_seatedSinceMs = nowMs;
			return EntryStuck::SeatedOnFoot;
		}

		// 3. An entry of the player's own going nowhere. Only with his
		// controls on: a script that walks him to a car with the controls
		// off can keep him at the door on purpose (the engine's own
		// passenger timer for a mission char is fourteen seconds, 0x004D85EF),
		// and it will warp him in at the end of it.
		const bool trying = !p.inVehicle && p.controlsOn && !ownEntry &&
		                    ((p.enterObjective && p.seeking) || p.entering);
		if (!trying) {
			Anchor(p, nowMs);
			return EntryStuck::None;
		}
		const float dx = p.x - m_x, dy = p.y - m_y;
		const bool  moved = dx * dx + dy * dy > ENTRY_MOVED_M * ENTRY_MOVED_M ||
		                   (p.entering && p.animMark != m_mark) || p.entering != m_entering;
		if (moved) {
			Anchor(p, nowMs);
			return EntryStuck::None;
		}
		if (nowMs - m_movedMs >= ENTRY_STUCK_MS) {
			Anchor(p, nowMs);
			return EntryStuck::NoProgress;
		}
		return EntryStuck::None;
	}

	void Reset() { m_polled = false; }

private:
	void Anchor(const LocalEntryProbe &p, uint32_t nowMs) {
		m_x        = p.x;
		m_y        = p.y;
		m_mark     = p.animMark;
		m_entering = p.entering;
		m_movedMs  = nowMs;
	}

	bool     m_polled        = false;
	uint32_t m_lastMs        = 0;
	uint32_t m_staleSinceMs  = 0;
	uint32_t m_seatedSinceMs = 0;
	uint32_t m_movedMs       = 0;
	float    m_x = 0.0f, m_y = 0.0f;
	uint32_t m_mark     = 0;
	bool     m_entering = false;
};

// The session's record of our seat against what our engine says, for
// SESSION_SEAT_STALE_MS. `held` is a seat the session has told us we hold;
// `onFoot` is the engine's answer, getting in and getting out not counted.
// True once when the two have disagreed for long enough; it then waits for
// the record to change before it can say so again.
class SessionSeatWatch {
public:
	bool Tick(uint16_t held, bool onFoot, bool ownEntry, uint32_t nowMs) {
		if (m_polled && nowMs - m_lastMs > ENTRY_WATCH_GAP_MS)
			m_sinceMs += nowMs - m_lastMs;
		m_polled = true;
		m_lastMs = nowMs;
		if (held != m_held) {
			m_held  = held;
			m_fired = false;
			m_sinceMs = nowMs;
		}
		if (held == INVALID_NETID || !onFoot || ownEntry || m_fired) {
			m_sinceMs = nowMs;
			return false;
		}
		if (nowMs - m_sinceMs < SESSION_SEAT_STALE_MS)
			return false;
		m_fired = true;
		return true;
	}

private:
	bool     m_polled  = false;
	bool     m_fired   = false;
	uint16_t m_held    = INVALID_NETID;
	uint32_t m_lastMs  = 0;
	uint32_t m_sinceMs = 0;
};

} // namespace coopiii
