// CoopIII.asi - entry point.
//
// Loaded by whatever ASI loader the player has (docs/compat.md §2.1). One
// rule here: DllMain does almost nothing. It starts a thread and returns.
//
// Why (docs/compat.md §2.2): DllMain runs under the loader lock, and at that
// point SilentPatch, the Widescreen Fix and Framerate Vigilante haven't
// patched anything yet - Mod Loader hasn't even run. Installing a detour
// there would mean racing mods the player chose to install, over the same
// bytes. The boot thread waits instead, until the game loop is actually
// running, which is the one point we can be sure everyone else is done.
#include "client.h"
#include "clock.h"
#include "config.h"
#include "game/combat.h"
#include "game/frame.h"
#include "game/nametag.h"
#include "game/pause.h"
#include "game/ped.h"
#include "game/radar.h"
#include "game/vehicle.h"
#include "game/verify.h"
#include "game/world.h"
#include "game/worldstate.h"
#include "hook/hook.h"
#include "log.h"

#include <windows.h>

using namespace coopiii;

namespace {

Config g_config;
Client g_client;
bool   g_started = false;

// Measured behaviour: the frame counter first moves once a game is loaded,
// so this is how long we're willing to wait on the title screen for the
// player to pick a save. 60s was wrong, and made a normal menu look like a
// startup failure.
//
// Whether that's what the counter is actually *supposed* to do is a separate
// question - static analysis of the retail image says it should also tick in
// the frontend. See the long note in game/frame.cpp WaitForGameLoop for that.
// Either way, 20 minutes is the right number for CoopIII, which wants a
// running game before it installs anything.
constexpr uint32_t GAME_LOOP_TIMEOUT_MS = 20 * 60 * 1000;   // 20 minutes

void PreFrame() {
	// Before anything else this frame, before CGame::Process even decides
	// whether to update the world. game/pause.h has the full reasoning.
	game::ClearPauseForTheWorld();
	g_client.PreFrame();

	// Right after the roster, so the radar agrees with the peds that were
	// just spawned or destroyed. Not from the HUD draw: game/radar.cpp says
	// why a blip has to be released even on a frame with no radar on screen.
	game::UpdateRemoteBlips();

	// Last, after the roster has spawned and despawned whatever it was going
	// to: look at the list CWorld::Process is about to walk and take out
	// anything it would fault on. game/world.h says why this exists.
	game::GuardMovingList();
}

void PostFrame() {
	g_client.PostFrame();

	// Last, so the flag agrees with the menu again by the time
	// cAudioManager::Service and the HUD read it - both run after
	// CGame::Process returns.
	game::RestorePauseForPresentation();

	// Heartbeat - proof the hook is still live rather than silently
	// detached, which is exactly the failure this whole file exists to
	// avoid. Once a minute at 60 FPS, so it costs nothing and still puts a
	// timestamp in the log.
	const uint32_t frames = game::FramesSeen() + 1;
	if (frames == 1 || frames % 3600 == 0)
		Log("frame: %u frames, %s, %u remote player(s), %u ms rtt", frames,
		    g_client.IsConnected() ? "connected" : "not connected",
		    g_client.RemoteCount(), g_client.RoundTripMs());
}

DWORD WINAPI Boot(LPVOID) {
	WallClock::Start();

	g_config.LoadFromFile(Config::IniPath());
	g_config.ApplyEnvOverrides();
	if (g_config.logToFile)
		LogOpen(Config::PathNextToModule("CoopIII.log"));

	Log("CoopIII starting (pid %lu)", GetCurrentProcessId());
	if (g_config.logToFile && LogPath() != Config::PathNextToModule("CoopIII.log"))
		Log("log: another instance already holds CoopIII.log, so this one is writing to "
		    "\"%s\". Two processes appending to one log interleave into nonsense.",
		    LogPath().c_str());
	Log("config: server %s:%u, nick \"%s\" (from %s)", g_config.host.c_str(), g_config.port,
	    g_config.nick.c_str(), Config::IniPath().c_str());

	const game::VerifyResult image = game::VerifyGameImage();
	Log("image: %s", image.detail.c_str());
	if (!image.ok) {
		Log("CoopIII will not load. No hooks were installed and the game is "
		    "untouched.");
		return 0;
	}

	if (!game::WaitForGameLoop(GAME_LOOP_TIMEOUT_MS)) {
		Log("no game was started within %u minutes, giving up without "
		    "installing anything. Start or load a game and relaunch.",
		    GAME_LOOP_TIMEOUT_MS / 60000);
		return 0;
	}
	Log("game loop is running; other plugins have finished loading");

	if (!HookInit()) {
		for (const auto &f : HookFailures())
			Log("hook init: %s: %s", f.name.c_str(), f.reason.c_str());
		return 0;
	}

	if (!game::InstallFrameHook(&PreFrame, &PostFrame)) {
		HookShutdown();
		return 0;
	}
	g_started = true;

	// Not fatal if this fails - the game just pauses the way it does in
	// single player, which is wrong for a session but not a broken game.
	if (g_config.menuPausesTheGame)
		Log("pause: menuPausesTheGame is on, so the menu stops the world as it "
		    "does in single player. Everyone else keeps playing");
	else
		game::InstallPausePolicy();

	// Combat is sampled by detour, not by the frame pump - a shot is an
	// event, and only the engine knows when one actually happened. Not fatal
	// if this fails either: a session with no muzzle flashes and no synced
	// grenades is a worse session, not a broken game, and the log says why.
	//
	// The one worth reading the log for is CPed::InflictDamage. Without it
	// nothing can hurt a remote player, and the proof flags on their ped go
	// back to being the only thing keeping the local engine from deciding
	// their health.
	if (!game::InstallCombatHooks())
		Log("CoopIII: combat is not fully hooked; firing, explosions and damage "
		    "may not reach other players");

	// Same story for a car blowing up, and the same reason it is a detour
	// rather than a field: nothing in the engine watches a car's health for
	// zero, so an observer handed a health of zero gets an undamaged-looking
	// car with no health instead of a wreck. Not fatal - without it the
	// destruction neither travels nor gets held back, which is the behaviour
	// this replaces, and the log says so.
	if (!game::InstallVehicleHooks())
		Log("CoopIII: a car exploding will not reach other players");

	WorldBridge bridge = game::MakeWorldBridge();
	// Clock and weather are wired here rather than inside MakeWorldBridge
	// because they share nothing with the ped and vehicle code: different
	// addresses, different file, no entities involved.
	game::AddWorldToBridge(bridge);
	game::AddVehicleBlastToBridge(bridge);

	if (!g_client.Start(g_config.host, g_config.port, g_config.nick, bridge)) {
		Log("CoopIII: the network client failed to start; the frame hook stays "
		    "installed but nothing will be sent");
		return 0;
	}

	// Nametags hang off CHud::Draw rather than the frame pump, because they're
	// drawn in the render pass and everything the frame pump reaches happens
	// before it (game/nametag.cpp). Last, and only once there's a session for
	// them to label, so a client that never starts doesn't leave a detour on
	// the HUD with nothing to draw. Not fatal if it fails: the session still
	// works, you just can't tell who is who.
	game::SetNametagScale(g_config.nametagScale);
	game::InstallNametags(g_client);

	// Blips need no detour at all - CHud::Draw already runs CRadar::DrawBlips
	// over the game's own blip table every frame, so CoopIII only keeps the
	// table right. This checks the table is really a blip table first, and
	// refuses loudly rather than writing 32 slots of somebody else's memory.
	game::InstallRadarBlips(g_client);

	Log("CoopIII ready");
	return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH: {
		// We never care about thread attach/detach, and the game creates plenty.
		DisableThreadLibraryCalls(module);

		const HANDLE thread = CreateThread(nullptr, 0, &Boot, nullptr, 0, nullptr);
		if (thread)
			CloseHandle(thread);
		break;
	}
	case DLL_PROCESS_DETACH:
		// On process exit, Windows may have already torn other threads down,
		// so unhooking is only really safe on an explicit unload. Either way
		// the process is going away regardless - keep this minimal.
		if (g_started) {
			// First, because the draw reads the roster straight out of the
			// client and the client is about to be stopped.
			game::RemoveNametags();
			// Before the roster goes away, while the ped refs a blip is
			// recognised by are still the ones we registered.
			game::RemoveRadarBlips();
			g_client.Stop();
			game::RemovePausePolicy();
			// Before the frame hook, and before MinHook goes away. Removing
			// the combat detours ends every projectile CoopIII was animating
			// for somebody else, and that has to happen while the detour
			// keeping them from exploding is still installed.
			game::RemoveCombatHooks();
			// Same reason, and it also puts CVehicleModelInfo::ms_compsToUse
			// back to { -2, -2 }. Leaving a component override behind would
			// have the game fit it to the next car it creates by itself, for
			// the rest of the session, with CoopIII gone and nothing left to
			// explain it.
			game::RemoveVehicleHooks();
			game::RemoveFrameHook();
			HookShutdown();
		}
		LogClose();
		break;
	}
	return TRUE;
}
