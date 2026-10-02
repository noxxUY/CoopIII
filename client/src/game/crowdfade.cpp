// The engine half of game/crowdfade.h: our camera as the engine has it, whether
// a copy is on our screen by the engine's own test, and a copy's clump alpha
// with bFadeOut, written the way the generators and the reapers write their
// own. Hooks nothing; addresses.h has every address and the instructions that
// prove it, and tools/clienttest/crowdfade.cpp reads them back out of the exe.
#include "crowdfade.h"

#include "addresses.h"
#include "vehicle.h"
#include "../client.h"

namespace coopiii::game {

namespace {

using PedFn       = void *(__cdecl *)();
using GetPedFn    = void *(__cdecl *)(int32_t);
using OnScreenFn  = bool(__thiscall *)(void *);
using SetAlphaFn  = void(__cdecl *)(void *, int32_t);

// The copy a row's pool handle names, of the kind the row says.
void *ReplicaEntity(uint8_t kind, int32_t handle) {
	if (handle < 0)
		return nullptr;
	if (kind == AMBIENT_ADOPT_CAR)
		return AmbientCarFromRef(handle);
	return Func<GetPedFn>(CPools__GetPed)(handle);
}

bool SampleLocalView(PlayerViewBody &out) {
	if (!Func<PedFn>(FindPlayerPed)())
		return false;
	const float *pos = Ptr<float>(TheCamera__Position);
	const float *fwd = Ptr<float>(TheCamera__Forward);
	out.pos = Vec3{pos[0], pos[1], pos[2]};
	out.fwd = Vec3{fwd[0], fwd[1], fwd[2]};
	return PlayerViewSane(out);
}

// CEntity::GetIsOnScreen, the test PossiblyRemoveVehicle and ManagePopulation
// both put a car or a pedestrian of their own to before deciding between
// taking it at once and fading it out.
bool CrowdReplicaOnScreen(uint8_t kind, int32_t handle) {
	void *const e = ReplicaEntity(kind, handle);
	if (!e || !Field<void *>(e, offs::RW_OBJECT))
		return false;
	return Func<OnScreenFn>(CEntity__GetIsOnScreen)(e);
}

// The alpha as our clock has it, and bFadeOut so the engine's own blend in
// ProcessControl, if it runs on this copy this frame, goes the same way: down
// by 8 while fading out, up by 16 otherwise.
void SetCrowdReplicaAlpha(uint8_t kind, int32_t handle, uint8_t alpha, bool fadingOut) {
	void *const e = ReplicaEntity(kind, handle);
	if (!e)
		return;
	void *const clump = Field<void *>(e, offs::RW_OBJECT);
	if (!clump)
		return;
	Func<SetAlphaFn>(CVisibilityPlugins__SetClumpAlpha)(clump, alpha);
	uint8_t &flags = kind == AMBIENT_ADOPT_CAR ? Field<uint8_t>(e, offs::VEH_FLAGS_C)
	                                           : Field<uint8_t>(e, offs::PED_FLAGS_G);
	const uint8_t bit = kind == AMBIENT_ADOPT_CAR ? offs::VEH_FADE_OUT : offs::PED_FADE_OUT;
	flags = static_cast<uint8_t>(fadingOut ? (flags | bit) : (flags & ~bit));
}

} // namespace

void AddCrowdFadeToBridge(WorldBridge &bridge) {
	bridge.SampleLocalView      = &SampleLocalView;
	bridge.CrowdReplicaOnScreen = &CrowdReplicaOnScreen;
	bridge.SetCrowdReplicaAlpha = &SetCrowdReplicaAlpha;
}

} // namespace coopiii::game
