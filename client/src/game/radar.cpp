// Putting remote players on the radar. radar.h has the design and the
// reasoning; this is the half that touches game memory.
//
// ---------------------------------------------------------------------------
// There is no hook in this file, and that is the point
// ---------------------------------------------------------------------------
//
// CHud::Draw already calls CRadar::DrawMap and then CRadar::DrawBlips, once a
// frame, and DrawBlips already walks all 32 entries of CRadar::ms_RadarTrace
// and draws whatever it finds. So a remote player appears on the minimap the
// moment there is an entry for them, drawn by the game's own code, on the
// game's own schedule, with the game's own colours.
//
// Which also means this file must not touch CHud::Draw: nametag.cpp already
// detours it, MinHook allows one hook per target, and a second one would
// either fail to install or silently displace the tags.
//
// ---------------------------------------------------------------------------
// Why the reconcile runs from PreFrame rather than from the draw
// ---------------------------------------------------------------------------
//
// Hooking CRadar::DrawBlips was the obvious alternative and it is worse in
// one specific way. DrawBlips is called from inside a conditional in
// CHud::Draw, and the radar is not drawn at all when the player has the HUD
// off, during a cutscene, or while the radar is the flashing HUD item. A
// reconcile that only runs when the radar is drawn would leave blips for
// players who have left sitting in the table for as long as the HUD is
// hidden - invisible, but occupying slots out of 32 that the campaign script
// needs, which is the resource this file is most careful with.
//
// PreFrame runs every frame regardless, and it runs after Client::PreFrame
// has finished spawning and despawning peds, so the roster and the peds agree
// by the time this looks at them.
//
// ---------------------------------------------------------------------------
// Nothing here changes a ped
// ---------------------------------------------------------------------------
//
// Same rule as nametag.cpp. This resolves each player's ped read-only and
// does not call ped.cpp's ResolveRemote, because that clears the handle and
// re-arms the spawn when a ped has gone - correct for the network path, and
// not something a radar update gets to decide. If the ped is not there this
// frame there is no blip this frame, and Client::PreFrame sorts it out on its
// own schedule.
#include "radar.h"

#include "client.h"
#include "log.h"

#include <cstdint>

namespace coopiii::game {

namespace {

using GetPedFn        = void *(__cdecl *)(int32_t);
// All __cdecl, all four arguments a dword wide - the retail call site cleans
// `add esp,10h` after four pushes, and the callee reads them at [esp+8],
// [esp+0Ch], [esp+10h] and [esp+14h]. m_eBlipDisplay is a word in the table,
// but the argument that fills it is not.
using SetEntityBlipFn = int32_t(__cdecl *)(uint32_t, int32_t, uint32_t, uint32_t);
using ClearBlipFn     = void(__cdecl *)(int32_t);
using ChangeBlipFn    = void(__cdecl *)(int32_t, int32_t);

const Client *g_client    = nullptr;
bool          g_installed = false;

// What CoopIII believes it owns on the radar, per roster slot. Kept here
// rather than on RemotePlayer for the same reason nametag.cpp keeps its
// occlusion state here: it is nothing to do with the roster. It is a fact
// about this machine's radar, and client.h belongs to another agent.
struct OurBlip {
	int32_t  handle = NO_BLIP;
	uint32_t colour = 0xFFFFFFFFu;   // not a trace colour: "nothing applied"
	int32_t  pedRef = -1;            // the ped it was made for
};
OurBlip g_blips[MAX_PLAYERS];

// Said once each, never per frame. With this many mods patching the same
// binary, a feature that quietly does nothing is the failure to design
// against - but a log full of one line is no use either.
bool g_saidFirstBlip = false;
bool g_saidNoSlots   = false;
bool g_saidLostBlip  = false;

// ---- the table ------------------------------------------------------------

template <class T>
T &Trace(int slot, size_t offset) {
	return *reinterpret_cast<T *>(CRadar__ms_RadarTrace +
	                              static_cast<uintptr_t>(slot) * SIZEOF_RADAR_TRACE +
	                              offset);
}

bool TraceInUse(int slot) {
	return Trace<uint8_t>(slot, TRACE_IN_USE) != 0;
}

// Free slots, not counting the ones CoopIII is already using - those are in
// use, so they are not free, which is exactly what MayTakeTraceSlot expects.
int CountFreeTraceSlots() {
	int free = 0;
	for (int slot = 0; slot < static_cast<int>(NUM_RADAR_BLIPS); ++slot) {
		if (!TraceInUse(slot))
			++free;
	}
	return free;
}

// Does the slot CoopIII thinks it owns still hold CoopIII's blip? See
// TraceIsOurs in radar.h for why this asks about the contents rather than
// validating the handle.
bool StillOurs(const OurBlip &ours) {
	if (!BlipHandleUsable(ours.handle))
		return false;
	const int slot = BlipSlot(ours.handle);
	return TraceIsOurs(TraceInUse(slot), Trace<uint32_t>(slot, TRACE_BLIP_TYPE),
	                   Trace<int32_t>(slot, TRACE_ENTITY_HANDLE), ours.pedRef);
}

// ---- the engine's blip calls ----------------------------------------------

int32_t SetEntityBlipForPed(int32_t pedRef, uint32_t colour) {
	return Func<SetEntityBlipFn>(CRadar__SetEntityBlip)(
	    BLIP_CHAR, pedRef, colour, uint32_t(BLIP_DISPLAY_BLIP_ONLY));
}

void ClearBlip(int32_t handle) {
	Func<ClearBlipFn>(CRadar__ClearBlip)(handle);
}

// ADD_BLIP_FOR_CHAR's own scale, and it has to be set after the fact because
// SetEntityBlip writes m_wScale = 1 itself. ShowRadarTrace draws the square
// from -size to +size with a one-wider black border, so 1 is about four
// pixels across at the HUD's reference resolution - which is why the script
// never leaves it there for anything it wants you to find.
void ChangeBlipScale(int32_t handle, int32_t scale) {
	Func<ChangeBlipFn>(CRadar__ChangeBlipScale)(handle, scale);
}

void ChangeBlipColour(int32_t handle, uint32_t colour) {
	Func<ChangeBlipFn>(CRadar__ChangeBlipColour)(handle, static_cast<int32_t>(colour));
}

constexpr int32_t BLIP_SCALE_PLAYER = 3;

// ---- the ped --------------------------------------------------------------

// A remote player's live CPed, read-only. The vtable check is nametag.cpp's,
// and it is what turns a recycled pool slot into a null rather than into a
// blip following a pedestrian around.
void *LivePed(const RemotePlayer &player) {
	if (!player.active || player.poolHandle < 0)
		return nullptr;
	void *ped = Func<GetPedFn>(CPools__GetPed)(player.poolHandle);
	if (ped == nullptr)
		return nullptr;
	if (Field<uintptr_t>(ped, offs::VTABLE) != CCivilianPed__vtable)
		return nullptr;
	return ped;
}

// Exactly the test DrawBlips itself makes before deciding to draw a
// BLIP_CHAR at the car's position instead of the ped's: `cmp byte
// [eax+314h],0 / je` then `mov edi,[eax+310h] / test edi,edi`. Reading the
// same two fields is what keeps the colour and the position from ever
// disagreeing about whether somebody is driving.
bool PedIsInVehicle(void *ped) {
	return Field<bool>(ped, offs::PED_IN_VEHICLE) &&
	       Field<void *>(ped, offs::PED_MY_VEHICLE) != nullptr;
}

// ---- one player -----------------------------------------------------------

void DropOurRecord(OurBlip &ours) {
	ours.handle = NO_BLIP;
	ours.colour = 0xFFFFFFFFu;
	ours.pedRef = -1;
}

// Gives up the slot and says so. Only for blips CoopIII still owns - a slot
// that has stopped being ours is the script's now and must not be cleared.
//
// The pool check is the teardown case. ClearBlip calls SetRadarMarkerState,
// which resolves the entity out of CPools::ms_pPedPool to clear its bHasBlip,
// and RemoveRadarBlips runs from DLL_PROCESS_DETACH where the engine may
// already have shut its pools down. A null pool pointer there means there is
// nothing left to tidy and a live one means it is safe to.
void ReleaseBlip(OurBlip &ours) {
	if (StillOurs(ours) && Global<void *>(CPools__ms_pPedPool) != nullptr)
		ClearBlip(ours.handle);
	DropOurRecord(ours);
}

// Pass one for one player: give the slot back if it should not be ours, and
// notice if it has stopped being ours without anyone saying so. Runs for
// every player before anything is counted, so a slot let go of this frame is
// available again this frame.
void ReleaseIfUnwanted(uint8_t id, const RemotePlayer &player, bool connected) {
	OurBlip &ours = g_blips[id];
	if (ours.handle == NO_BLIP)
		return;

	void *const ped = connected ? LivePed(player) : nullptr;
	const bool  want =
	    connected && BlipWanted(player.active, player.haveState, ped != nullptr);

	// Stopped being ours: the ped died and ~CPed cleared the blip, the script
	// wiped the table on a game load, or a slot we let go of got recycled.
	// Forget it rather than clearing it - by now the slot may be somebody
	// else's.
	if (!StillOurs(ours)) {
		// Only worth a line when we still wanted it. A blip that went away
		// because the player left is the system working.
		if (want && !g_saidLostBlip) {
			g_saidLostBlip = true;
			Log("radar: the blip for player %u is gone from slot %d - the ped was "
			    "destroyed or the game reused the slot. It will be put back, and "
			    "this is only reported once",
			    id, BlipSlot(ours.handle));
		}
		DropOurRecord(ours);
		return;
	}

	// A ped that was destroyed and respawned is a different ped with a
	// different ref, and a blip pointed at the old one tracks nothing.
	if (!want || ours.pedRef != player.poolHandle)
		ReleaseBlip(ours);
}

// Pass two for one player: take a slot if they should have one and do not,
// and keep the colour honest if they do.
void EnsureBlip(uint8_t id, const RemotePlayer &player, int &freeSlots,
                int &oursHeld) {
	OurBlip &ours = g_blips[id];

	void *const ped = LivePed(player);
	if (!BlipWanted(player.active, player.haveState, ped != nullptr))
		return;

	const uint32_t colour = BlipColourFor(PedIsInVehicle(ped));

	if (ours.handle != NO_BLIP) {
		++oursHeld;
		// On change only. ChangeBlipColour is cheap, but so is the compare,
		// and writing a field every frame is how a bug hides in the noise.
		if (colour != ours.colour) {
			ChangeBlipColour(ours.handle, colour);
			ours.colour = colour;
		}
		return;
	}

	if (!MayTakeTraceSlot(freeSlots, oursHeld)) {
		if (!g_saidNoSlots) {
			g_saidNoSlots = true;
			Log("radar: only %d of the game's %u blip slots are free and %d are "
			    "already ours, so player %u gets no blip. The campaign script owns "
			    "this table and CoopIII will not take the last of it; reported once",
			    freeSlots, unsigned(NUM_RADAR_BLIPS), oursHeld, id);
		}
		return;
	}

	const int32_t handle = SetEntityBlipForPed(player.poolHandle, colour);
	if (!BlipHandleUsable(handle)) {
		// Cannot happen with a free slot in hand, which is the point of
		// counting first - but if it ever does, the alternative is writing
		// through a handle for a slot that does not exist, and the slot after
		// the last one is CDarkel's kill register.
		Log("radar: CRadar::SetEntityBlip returned 0x%08X for player %u, which is "
		    "not a usable blip handle; leaving them off the radar",
		    static_cast<unsigned>(handle), id);
		return;
	}

	ChangeBlipScale(handle, BLIP_SCALE_PLAYER);

	ours.handle = handle;
	ours.colour = colour;
	ours.pedRef = player.poolHandle;
	--freeSlots;
	++oursHeld;

	if (!g_saidFirstBlip) {
		g_saidFirstBlip = true;
		Log("radar: player %u (\"%s\") is on the minimap - slot %d, %s, scale %d",
		    id, player.nick.c_str(), BlipSlot(handle),
		    colour == RADAR_TRACE_RED ? "red (in a vehicle)" : "green (on foot)",
		    BLIP_SCALE_PLAYER);
	}
}

} // namespace

// ---- installing -----------------------------------------------------------

bool InstallRadarBlips(const Client &client) {
	for (OurBlip &blip : g_blips)
		blip = OurBlip{};
	g_saidFirstBlip = false;
	g_saidNoSlots   = false;
	g_saidLostBlip  = false;
	g_installed     = false;
	g_client        = nullptr;

	// Before writing a single entry, check the table looks like CRadar's.
	// verify.cpp has already refused to load against any other image, so a
	// failure here means something patched or moved the table at runtime -
	// and the consequence of writing it anyway is not a missing blip, it is
	// 32 arbitrary slots of somebody else's memory.
	int bad   = 0;
	int inUse = 0;
	for (int slot = 0; slot < static_cast<int>(NUM_RADAR_BLIPS); ++slot) {
		const bool used = TraceInUse(slot);
		if (used)
			++inUse;
		if (!TraceEntrySane(used, Trace<uint32_t>(slot, TRACE_BLIP_TYPE),
		                    Trace<uint16_t>(slot, TRACE_BLIP_DISPLAY),
		                    Trace<uint16_t>(slot, TRACE_RADAR_SPRITE)))
			++bad;
	}

	if (bad != 0) {
		Log("radar: NOT putting players on the minimap. %d of the %u entries at "
		    "CRadar::ms_RadarTrace (0x%08X) do not look like radar blips, so "
		    "something has moved or patched the table and writing it would "
		    "corrupt whatever is really there",
		    bad, unsigned(NUM_RADAR_BLIPS), unsigned(CRadar__ms_RadarTrace));
		return false;
	}

	g_client    = &client;
	g_installed = true;
	Log("radar: remote players will be blips on the minimap. "
	    "CRadar::ms_RadarTrace at 0x%08X, %u slots of 0x%02X, %d in use by the "
	    "game right now, %d reserved for it",
	    unsigned(CRadar__ms_RadarTrace), unsigned(NUM_RADAR_BLIPS),
	    unsigned(SIZEOF_RADAR_TRACE), inUse, RADAR_SCRIPT_RESERVE);
	return true;
}

void UpdateRemoteBlips() {
	if (!g_installed || g_client == nullptr)
		return;

	// Not connected is not the same as no players: dropping the session has
	// to take the blips with it, or they sit on the radar tracking peds that
	// are about to be destroyed.
	const bool    connected = g_client->IsConnected();
	const uint8_t localId   = g_client->LocalPlayerId();

	// Two passes, and the order is the point. Releasing first means a slot
	// given up this frame - a player who left, a ped that was rebuilt - is
	// free again by the time the count below is taken, instead of being
	// counted as the script's until the next frame.
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == localId) {
			// You are the centre sprite, not a blip.
			if (g_blips[id].handle != NO_BLIP)
				ReleaseBlip(g_blips[id]);
			continue;
		}
		ReleaseIfUnwanted(id, g_client->PlayerSlot(id), connected);
	}

	if (!connected)
		return;

	int freeSlots = CountFreeTraceSlots();
	int oursHeld  = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == localId)
			continue;
		EnsureBlip(id, g_client->PlayerSlot(id), freeSlots, oursHeld);
	}
}

void RemoveRadarBlips() {
	for (OurBlip &blip : g_blips)
		ReleaseBlip(blip);
	g_installed = false;
	g_client    = nullptr;
}

bool RadarBlipsInstalled() {
	return g_installed;
}

} // namespace coopiii::game
