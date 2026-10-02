// The engine half of game/crane.h.
#include "crane.h"

#include "carremoval.h"
#include "teardown.h"
#include "vehicle.h"
#include "../clock.h"
#include "../log.h"

#include <cmath>

namespace coopiii::game {

namespace {

using ThisFn     = void(__thiscall *)(void *);
using RegisterFn = void(__thiscall *)(void *, void **);

// Another machine's crane, as last heard, for each of ours.
struct Follow {
	bool           on        = false;
	uint8_t        from      = INVALID_PLAYER;
	uint32_t       heardMs   = 0;   // our clock
	uint32_t       sentMs    = 0;   // theirs
	bool           haveEnd   = false;
	uint8_t        endFrom   = INVALID_PLAYER;
	uint32_t       endSentMs = 0;
	CraneStateBody body{};
};

Follow   g_follow[NUM_CRANES];
// Whether our own engine had the crane busy on its last Update, and the ones
// that have gone idle since and still have to say so.
bool     g_busy[NUM_CRANES] = {};
uint8_t  g_endPending       = 0;

bool g_saidFollow    = false;
bool g_saidLapsed    = false;
bool g_saidStoodDown = false;
bool g_saidDropped   = false;
bool g_saidCrushed   = false;

int32_t CraneCount() {
	const int32_t n = Global<int32_t>(CCranes__NumCranes);
	return n < 0 ? 0 : (n > NUM_CRANES ? NUM_CRANES : n);
}

void *CraneAt(int32_t i) {
	return Ptr<uint8_t>(CCranes__aCranes) + static_cast<size_t>(i) * SIZEOF_CRANE;
}

int32_t IndexOf(void *crane) {
	const intptr_t d = reinterpret_cast<uint8_t *>(crane) - Ptr<uint8_t>(CCranes__aCranes);
	if (d < 0 || d % static_cast<intptr_t>(SIZEOF_CRANE) != 0)
		return -1;
	const intptr_t i = d / static_cast<intptr_t>(SIZEOF_CRANE);
	return i < NUM_CRANES ? static_cast<int32_t>(i) : -1;
}

void *CarOf(void *crane) { return Field<void *>(crane, offs::CRANE_CAR); }

// Takes the car off the crane's pointer the way the garage target is taken
// (game/carremoval.cpp): nulled, and the reference FindCarInSectorList
// registered pruned, so the car's own end later does not reach back into a
// crane that has moved on. A car taken off the hook gets its collision back,
// which the crane would only have given it at the drop.
void LetGoOfCar(void *crane, bool hanging) {
	void *&slot     = Field<void *>(crane, offs::CRANE_CAR);
	void *const car = slot;
	slot            = nullptr;
	if (!car || !VehicleIsLive(car))
		return;
	Func<ThisFn>(CEntity__PruneReferences)(car);
	if (hanging)
		Field<uint8_t>(car, offs::ENTITY_FLAGS_A) |= offs::ENTITY_USES_COLLISION;
}

// Names our copy of the car another machine's crane has, with a reference of
// its own, so a copy that goes nils the pointer as the engine's own does.
void HoldCar(void *crane, void *car) {
	void *&slot = Field<void *>(crane, offs::CRANE_CAR);
	if (slot == car)
		return;
	LetGoOfCar(crane, false);
	if (!car || !VehicleIsLive(car))
		return;
	slot = car;
	Func<RegisterFn>(CEntity__RegisterReference)(car, &slot);
}

// The end of CCrane::Update, which every frame of it runs (addresses.h,
// 0x005445F6): the building turned to the hook's angle, and the hook put
// where the crane says.
void PlaceCrane(void *crane) {
	void *const building = Field<void *>(crane, offs::CRANE_ENTITY);
	if (!building)
		return;
	const float angle = Field<float>(crane, offs::CRANE_HOOK_ANGLE);
	const float c = std::cos(angle), s = std::sin(angle);
	Field<float>(building, CRANE_ENTITY_RIGHT_X) = c;
	Field<float>(building, CRANE_ENTITY_FWD_Y)   = c;
	Field<float>(building, CRANE_ENTITY_RIGHT_Y) = s;
	Field<float>(building, CRANE_ENTITY_FWD_X)   = -s;
	Func<ThisFn>(CMatrix__UpdateRW)(reinterpret_cast<uint8_t *>(building) + offs::MATRIX);
	Func<ThisFn>(CEntity__UpdateRwFrame)(building);
	Func<ThisFn>(CCrane__SetHookMatrix)(crane);
}

// Another machine's crane, onto ours, in place of our own Update.
void FollowCrane(void *crane, const CraneStateBody &b) {
	Field<float>(crane, offs::CRANE_HOOK_ANGLE)        = b.hookAngle;
	Field<float>(crane, offs::CRANE_HOOK_OFFSET)       = b.hookOffset;
	Field<float>(crane, offs::CRANE_HOOK_HEIGHT)       = b.hookHeight;
	Field<float>(crane, offs::CRANE_HOOK_POS)          = b.hookX;
	Field<float>(crane, offs::CRANE_HOOK_POS + 4)      = b.hookY;
	Field<float>(crane, offs::CRANE_HOOK_POS + 8)      = b.hookHeight;
	Field<float>(crane, offs::CRANE_HOOK_VELOCITY)     = 0.0f;
	Field<float>(crane, offs::CRANE_HOOK_VELOCITY + 4) = 0.0f;
	Field<uint8_t>(crane, offs::CRANE_STATE)           = b.state;
	HoldCar(crane, SessionCarHere(b.netId));
	PlaceCrane(crane);
}

void StopFollowing(void *crane, Follow &f) {
	f.on = false;
	LetGoOfCar(crane, false);
	Field<uint8_t>(crane, offs::CRANE_STATE) = CRANE_IDLE;
}

void StartFollowing(void *crane, int32_t i) {
	// Ours was busy too: two machines' cranes took a car in the same round
	// trip, and the session kept theirs. Ours lets go and says nothing more.
	if (CarOf(crane) || g_busy[i]) {
		if (!g_saidStoodDown) {
			g_saidStoodDown = true;
			Log("cranes: crane %d here was busy as well; the session has another "
			    "machine working it, so ours lets go and follows (said once)",
			    static_cast<int>(i));
		}
		LetGoOfCar(crane, CraneCarries(Field<uint8_t>(crane, offs::CRANE_STATE)));
	}
	g_busy[i]     = false;
	g_endPending &= static_cast<uint8_t>(~(1u << i));
}

// What our own engine does with a crane: never go on with a car somebody
// else's machine has come to hold (somebody got in, or another machine settles
// it). That machine's engine moves it, and a crane setting it on its hook
// here would only fight the correction.
void KeepOffOthersCars(void *crane) {
	void *const car = CarOf(crane);
	if (!car || EngineMayTakeCar(car))
		return;
	uint8_t &state = Field<uint8_t>(crane, offs::CRANE_STATE);
	LetGoOfCar(crane, CraneCarries(state));
	if (state == CRANE_GOING_TOWARDS_TARGET)
		state = CRANE_IDLE;
	if (!g_saidDropped) {
		g_saidDropped = true;
		Log("cranes: a crane here had a car another machine holds now; it let go "
		    "(said once)");
	}
}

void Fill(CraneStateBody &out, void *crane, int32_t i, bool active) {
	out        = CraneStateBody{};
	out.crane  = static_cast<uint8_t>(i);
	out.active = active ? 1 : 0;
	if (void *const building = Field<void *>(crane, offs::CRANE_ENTITY)) {
		out.craneX = Field<float>(building, offs::POSITION);
		out.craneY = Field<float>(building, offs::POSITION + 4);
	}
	if (!active)
		return;
	out.state      = Field<uint8_t>(crane, offs::CRANE_STATE);
	out.netId      = SessionNetIdHere(CarOf(crane));
	out.hookAngle  = Field<float>(crane, offs::CRANE_HOOK_ANGLE);
	out.hookOffset = Field<float>(crane, offs::CRANE_HOOK_OFFSET);
	out.hookHeight = Field<float>(crane, offs::CRANE_HOOK_HEIGHT);
	out.hookX      = Field<float>(crane, offs::CRANE_HOOK_POS);
	out.hookY      = Field<float>(crane, offs::CRANE_HOOK_POS + 4);
}

// ---- the bridge -----------------------------------------------------------

uint8_t SampleCranesImpl(CraneStateBody *out, uint8_t max) {
	uint8_t n = 0;
	for (int32_t i = 0; i < CraneCount() && n < max; ++i) {
		void *const crane = CraneAt(i);
		if (g_follow[i].on || !Field<void *>(crane, offs::CRANE_ENTITY))
			continue;
		const uint8_t bit = static_cast<uint8_t>(1u << i);
		if (g_busy[i]) {
			Fill(out[n++], crane, i, true);
		} else if (g_endPending & bit) {
			g_endPending &= static_cast<uint8_t>(~bit);
			Fill(out[n++], crane, i, false);
		}
	}
	return n;
}

void ApplyCraneStateImpl(const CraneStateBody &b, uint8_t from, uint32_t sentMs) {
	for (int32_t i = 0; i < CraneCount(); ++i) {
		void *const crane    = CraneAt(i);
		void *const building = Field<void *>(crane, offs::CRANE_ENTITY);
		if (!building || !SameCrane(Field<float>(building, offs::POSITION),
		                            Field<float>(building, offs::POSITION + 4), b.craneX,
		                            b.craneY))
			continue;
		Follow &f = g_follow[i];
		if (b.active == 0) {
			if (f.on && f.from == from)
				StopFollowing(crane, f);
			f.haveEnd   = true;
			f.endFrom   = from;
			f.endSentMs = sentMs;
			return;
		}
		if (CraneStateIsOld(from, sentMs, f.haveEnd, f.endFrom, f.endSentMs, f.on, f.from,
		                    f.sentMs))
			return;
		if (!f.on) {
			StartFollowing(crane, i);
			if (!g_saidFollow) {
				g_saidFollow = true;
				Log("cranes: crane %d here follows player %u's (said once)",
				    static_cast<int>(i), static_cast<unsigned>(from));
			}
		}
		f.on      = true;
		f.from    = from;
		f.heardMs = WallClock::NowMs();
		f.sentMs  = sentMs;
		f.body    = b;
		return;
	}
}

bool CraneHasVehicleImpl(const RemoteVehicle &v) {
	void *const car = v.poolHandle < 0 ? nullptr : AmbientCarFromRef(v.poolHandle);
	if (!car)
		return false;
	for (int32_t i = 0; i < CraneCount(); ++i)
		if (!g_follow[i].on && CarOf(CraneAt(i)) == car)
			return true;
	return false;
}

void NoteCarCrushedElsewhereImpl(int32_t poolHandle) {
	Global<int32_t>(CGarages__CrushedCarId) = poolHandle;
	if (!g_saidCrushed) {
		g_saidCrushed = true;
		Log("cranes: a car was crushed on another machine; IS_CAR_CRUSHED here names "
		    "our handle %08X (said once)", static_cast<unsigned>(poolHandle));
	}
}

// The crusher crane makes the car it picks up collision-proof (0x00543E5E) and
// never takes it back, so the car drops into the crusher unhurt. A car we
// settle has that bit cleared every frame by the custody (TakeVehicleBack, the
// observed bit), so it is put back here, after the crane's Update and before
// CWorld::Process: while it hangs from our crusher crane, and for a while
// after the drop, by its handle, until the crusher has it.
constexpr uint32_t CRUSHER_DROP_PROOF_MS = 8000;
int32_t  g_droppedRef[NUM_CRANES]  = {-1, -1, -1, -1, -1, -1, -1, -1};
uint32_t g_droppedAtMs[NUM_CRANES] = {};

void MakeProof(void *car) {
	if (car && VehicleIsLive(car))
		Field<uint8_t>(car, offs::ENTITY_FLAGS_C) |= offs::ENTITY_COLLISION_PROOF;
}

void KeepCrushedCarProof(int32_t i, void *crane, void *carBefore, uint8_t stateBefore,
                         uint8_t state) {
	const uint32_t now = WallClock::NowMs();
	if (CraneCarries(state))
		MakeProof(CarOf(crane));
	if (carBefore && CraneCarries(stateBefore) && state == CRANE_DROPPING_TARGET &&
	    !CarOf(crane) && VehicleIsLive(carBefore)) {
		g_droppedRef[i]  = AmbientCarRef(carBefore);
		g_droppedAtMs[i] = now;
	}
	if (g_droppedRef[i] < 0)
		return;
	if (now - g_droppedAtMs[i] >= CRUSHER_DROP_PROOF_MS) {
		g_droppedRef[i] = -1;
		return;
	}
	MakeProof(AmbientCarFromRef(g_droppedRef[i]));
}

} // namespace

void __fastcall CraneUpdateHere(void *crane, void * /*edx*/) {
	const int32_t i = IndexOf(crane);
	if (i < 0) {
		Func<ThisFn>(CCrane__Update)(crane);
		return;
	}
	Follow &f = g_follow[i];
	if (f.on && CraneFollowLapsed(f.heardMs, WallClock::NowMs())) {
		if (!g_saidLapsed) {
			g_saidLapsed = true;
			Log("cranes: crane %d's worker has gone quiet; it is ours again (said once)",
			    static_cast<int>(i));
		}
		StopFollowing(crane, f);
	}
	if (f.on) {
		FollowCrane(crane, f.body);
		return;
	}
	KeepOffOthersCars(crane);
	void *const   carBefore   = CarOf(crane);
	const uint8_t stateBefore = Field<uint8_t>(crane, offs::CRANE_STATE);
	Func<ThisFn>(CCrane__Update)(crane);
	const uint8_t state = Field<uint8_t>(crane, offs::CRANE_STATE);
	const bool    busy  = CraneBusy(state, CarOf(crane) != nullptr);
	if (g_busy[i] && !busy)
		g_endPending |= static_cast<uint8_t>(1u << i);
	g_busy[i] = busy;
	if (Field<uint8_t>(crane, offs::CRANE_IS_CRUSHER) != 0)
		KeepCrushedCarProof(i, crane, carBefore, stateBefore, state);
}

void AddCranesToBridge(WorldBridge &bridge) {
	bridge.SampleCranes            = &SampleCranesImpl;
	bridge.ApplyCraneState         = &ApplyCraneStateImpl;
	bridge.CraneHasVehicle         = &CraneHasVehicleImpl;
	bridge.NoteCarCrushedElsewhere = &NoteCarCrushedElsewhereImpl;
}

void ForgetCranes() {
	for (int32_t i = 0; i < CraneCount(); ++i) {
		if (g_follow[i].on)
			StopFollowing(CraneAt(i), g_follow[i]);
		g_follow[i] = Follow{};
		g_busy[i]   = false;
		g_droppedRef[i] = -1;
	}
	g_endPending = 0;
}

} // namespace coopiii::game
