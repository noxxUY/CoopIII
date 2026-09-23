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
#include "game/cheats.h"
#include "game/combat.h"
#include "game/darkel.h"
#include "game/frame.h"
#include "game/garage.h"
#include "game/heli.h"
#include "game/heligun.h"
#include "game/liftbridge.h"
#include "game/lights.h"
#include "game/money.h"
#include "game/nametag.h"
#include "game/object.h"
#include "game/pause.h"
#include "game/ped.h"
#include "game/pickup.h"
#include "game/planes.h"
#include "game/population.h"
#include "game/radar.h"
#include "game/seat.h"
#include "game/sessionclock.h"
#include "game/trains.h"
#include "game/vehicle.h"
#include "game/wanted.h"
#include "game/verify.h"
#include "game/world.h"
#include "game/worldstate.h"
#include "hook/hook.h"
#include "log.h"

#include <cstdlib>

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

	// The moving-list sweep does NOT run from here any more when the
	// CWorld::Process detour took - it runs on entry to CWorld::Process,
	// with nothing between it and the dereference at 0x004B1B25.
	//
	// The reason is the whole point of game/world.h: a PreFrame sweep sits
	// before CGame::Process, and everything CGame::Process does before it
	// calls CWorld::Process - CTheScripts::Process, CPopulation, CCarCtrl,
	// the fire and explosion managers, and every CoopIII detour those reach
	// - is in the gap. Running it twice a frame would cost twice as much and
	// close nothing the later one does not.
	//
	// It stays here as the fallback, because a sweep that only runs before
	// the frame is still better than no sweep at all if the detour failed.
	if (!game::WorldProcessGuardInstalled())
		game::GuardMovingList();
}

void PostFrame() {
	g_client.PostFrame();

	// After CGame::Process, which is exactly where CPhysical::ProcessControl
	// has just finished deciding whether each loose object is asleep. Walks
	// the handful of objects this machine knocked over and owes everybody a
	// resting place for, and is a no-op on every frame nobody hit anything.
	game::TickUprootedObjects();

	// Last, so the flag agrees with the menu again by the time
	// cAudioManager::Service and the HUD read it - both run after
	// CGame::Process returns.
	game::RestorePauseForPresentation();

	// Heartbeat - proof the hook is still live rather than silently
	// detached, which is exactly the failure this whole file exists to
	// avoid. Once a minute at 60 FPS, so it costs nothing and still puts a
	// timestamp in the log.
	const uint32_t frames = game::FramesSeen() + 1;
	if (frames == 1 || frames % 3600 == 0) {
		Log("frame: %u frames, %s, %u remote player(s), %u ms rtt", frames,
		    g_client.IsConnected() ? "connected" : "not connected",
		    g_client.RemoteCount(), g_client.RoundTripMs());

		// Ped drops, and only once anything has happened. Three numbers
		// answer the only question worth asking in one glance, the same way
		// the combat lines do:
		// did our own peds drop anything, did we refuse the ones that are
		// not ours, and did anything arrive from anybody else. A feature
		// that does nothing has to say which of the three it stopped at.
		const game::PickupStats &p = game::GetPickupStats();
		if (p.dropsMade || p.dropsUnsent || p.dropsSuppressed ||
		    p.dropsReceived || p.dropsLost || p.dropsDuplicate)
			Log("frame: ped drops - %u shared, %u made with no session, %u "
			    "refused for somebody else's ped, %u built off the wire, %u "
			    "already there, %u lost to a full table",
			    p.dropsMade, p.dropsUnsent, p.dropsSuppressed,
			    p.dropsReceived, p.dropsDuplicate, p.dropsLost);

		// Breakable street objects, on the same rule: only once something
		// has happened, and the numbers chosen so a feature that is doing
		// nothing has to say which step it stopped at. `broken here` counts
		// every break this engine performed, including the ones applied off
		// the wire, so that number moving while `reported` and `from the
		// wire` stay at zero is the shape of "nothing is reaching anybody".
		const game::ObjectStats &o = game::GetObjectStats();
		if (o.breaksSeen || o.received)
			Log("frame: street objects - %u broken here, %u reported, %u from "
			    "the wire (%u had nothing here to break, %u already broken); "
			    "quiet for %u explosion(s), %u replica(s), %u unowned",
			    o.breaksSeen, o.reported, o.received, o.receivedUnmatched,
			    o.receivedNoop, o.skippedExplosion, o.skippedReplica,
			    o.skippedUnowned);

		// Uprooting, on its own line and on the same rule, because it is the
		// half that used to be missing and "breaking works and nothing falls
		// over" has to be readable as such. `came loose` moving while `rest
		// sent` stays at zero means objects are being knocked over and never
		// coming to rest here - which would be the watch table leaking or
		// the pool churning them away before they stop.
		if (o.uprootsSeen || o.restsReceived)
			Log("frame: uprooted objects - %u came loose (%u ours to follow, "
			    "%u somebody else's, %u in a blast), %u rest(s) sent, %u from "
			    "the wire (%u had nothing here, %u refused); %u lost before "
			    "they stopped, %u dropped to a full table",
			    o.uprootsSeen, o.uprootsWatched, o.uprootsNotOurs,
			    o.uprootsBlast, o.restsSent, o.restsReceived,
			    o.restsUnmatched, o.restsRefused, o.uprootsLost,
			    o.uprootsDropped);

		// What the moving-list sweep actually costs, measured on the machine
		// it runs on rather than asserted in a comment. It is on the game
		// thread once a frame, so this is the number that says whether it
		// may stay there.
		if (game::WorldProcessGuardInstalled())
			Log("frame: moving-list sweep - %u node(s) last frame, %u us last, "
			    "%u us worst", game::LastSweepNodes(), game::LastSweepMicros(),
			    game::WorstSweepMicros());
	}
}

DWORD WINAPI Boot(LPVOID) {
	WallClock::Start();

	g_config.LoadFromFile(Config::IniPath());
	g_config.ApplyEnvOverrides();
	if (g_config.logToFile)
		LogOpen(Config::PathNextToModule("CoopIII.log"));

	Log("CoopIII starting (pid %lu)", GetCurrentProcessId());

	// docs/roadmap.md §5.6: dropping CoopIII.asi into the game folder must not
	// change single player. The mod activates only when the game was started
	// by the launcher, which puts this marker in the child's environment;
	// environment blocks are inherited by CreateProcess children, so it needs
	// no IPC and cannot be set by accident.
	//
	// Started any other way, this returns here having installed nothing: no
	// hooks, no thread, no socket. One install serves both - the game launched
	// normally for single player, launched through CoopIII for co-op.
	if (const char *marker = std::getenv("COOPIII_LAUNCHED"); !marker || marker[0] == '\0') {
		Log("not started from the CoopIII launcher, so CoopIII is standing down. "
		    "Nothing was hooked and the game is untouched (roadmap §5.6).");
		Log("To play co-op, start the game from coopiii-launcher.exe.");
		return 0;
	}
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

	// The seatbelt on CWorld::Process, and the inspector it hands every
	// surviving entity to. Installed straight after the frame hook and
	// before anything that can create or destroy an entity, because the one
	// thing it must not do is start late.
	//
	// Not fatal: without it the sweep falls back to PreFrame, which is where
	// it was when it failed to catch three crashes in a row (game/world.h).
	game::SetMovingListEntityInspector(&game::ClampClumpAnimations);
	game::InstallWorldProcessGuard();

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

	// A remote player's arms up or down with their aim. Cosmetic: the shot
	// itself is aimed off C_Shot's direction whether this installs or not.
	game::InstallAimPitchHook();

	// Same story for a car blowing up, and the same reason it is a detour
	// rather than a field: nothing in the engine watches a car's health for
	// zero, so an observer handed a health of zero gets an undamaged-looking
	// car with no health instead of a wreck. Not fatal - without it the
	// destruction neither travels nor gets held back, which is the behaviour
	// this replaces, and the log says so.
	if (!game::InstallVehicleHooks())
		Log("CoopIII: a car exploding will not reach other players");

	// The one door into the world. Not fatal if it fails to install: CoopIII
	// then never notices an ambient pedestrian, which is where this project
	// was before docs/population.md existed. AddPopulationToBridge checks the
	// same thing and wires nothing if the door is shut.
	if (!game::InstallPopulationHooks())
		Log("CoopIII: ambient pedestrians stay local to each machine");

	// Cheats (game/cheats.h). Not fatal: without the first detour every cheat
	// runs where it was typed, which is what it always did; without the second
	// a riot can move a replica here until its owner's stream puts it back.
	if (!game::InstallCheatHooks())
		Log("CoopIII: cheats are not fully routed; see the cheats: lines above");

	// Money (game/money.h). Not fatal: without it a wrecked car pays whoever's
	// game watched it, which is what it always did, whatever the server says.
	if (!game::InstallMoneyHook())
		Log("CoopIII: rewards for wrecked cars stay with whoever's game saw them");

	// The El, the subway and the planes on the server's clock. Neither is
	// fatal: without them each machine runs its own trains and flies its own
	// planes, which is how it has always been.
	game::InstallTrainClock();
	game::InstallPlaneClock();
	// The traffic lights and the Shoreside lift bridge, the same way and just as
	// optional: without them each machine keeps its own lights and its own
	// bridge.
	game::InstallLightClock();
	game::InstallLiftBridgeClock();

	// The police helicopter (game/heli.h). Six detours, none fatal: without
	// them each wanted player's helicopter is his machine's alone, which is
	// how it has always been. Before MakeWorldBridge's callers below, because
	// AddHeliToBridge only offers replicas once the ProcessControl detour is
	// in - a replica without it would fly the real AI at this machine's player.
	if (!game::InstallHeliHooks())
		Log("CoopIII: the police helicopter is not fully shared; see the heli: "
		    "lines above for what that costs");
	// Its gun. One detour, on the function the helicopter fires through;
	// without it our own helicopter's rounds are only seen here. Drawing
	// somebody else's needs nothing hooked.
	if (!game::InstallHeliGunHook())
		Log("CoopIII: our police helicopter's gunfire stays on this machine");

	WorldBridge bridge = game::MakeWorldBridge();
	// Clock and weather are wired here rather than inside MakeWorldBridge
	// because they share nothing with the ped and vehicle code: different
	// addresses, different file, no entities involved.
	game::AddWorldToBridge(bridge);
	game::AddSessionClockToBridge(bridge);
	game::AddVehicleBlastToBridge(bridge);
	game::SetSeatKey(g_config.seatKey);
	game::AddSeatToBridge(bridge);
	game::AddPopulationToBridge(bridge);
	// The wanted level. Two reads and one write into the local player's own
	// CWanted, and nothing else: the police are ambient entities that
	// AddPopulationToBridge above has been replicating all along
	// (docs/wanted.md §4.2).
	game::InstallWantedBridge(bridge);
	// Rampages. Four entries and three detours; game/darkel.h is the design.
	game::InstallRampageBridge(bridge);
	game::AddHeliToBridge(bridge);
	game::AddHeliGunToBridge(bridge);
	game::AddCheatsToBridge(bridge);
	game::AddMoneyToBridge(bridge);

	// Pickups. One detour, on CPickups::Update, and it is the only way a
	// pickup can be collected in this build - docs/pickups.md 3 has the
	// whole-image scan that says so.
	//
	// Not fatal either, and the log line matters: without it every machine
	// keeps its own pickups, so two players can take the same shotgun and a
	// hidden package counts once per player. That is exactly the behaviour
	// this replaces, so a failure is a regression to it rather than a broken
	// game.
	if (!game::InstallPickupHook())
		Log("CoopIII: pickups are local to each machine; two players can take "
		    "the same one");

	// Doors, garages and the Pay'n'Spray. One detour, on CGarage::Update,
	// which is both how this machine finds out what its own state machine
	// decided and the only place a garage somebody else is using can be held.
	//
	// Not fatal: without it every garage stays local, which is the behaviour
	// this replaces - a garage that opens for one player is shut for
	// everybody else, including the safehouse door.
	if (!game::InstallGarageHook())
		Log("CoopIII: doors and garages are local to each machine");

	game::AddGaragesToBridge(bridge);

	game::AddPickupsToBridge(bridge);
	// The outbound half of the pickup seam, wired only once there is a
	// session to claim against. Captureless lambdas so these are plain
	// function pointers: the detour they are called from has no place to
	// keep state.
	{
		game::PickupCallbacks pickups;
		pickups.Claim = [](const PickupIdent &ident) {
			g_client.ClaimPickup(ident);
		};
		pickups.Release = [](const PickupIdent &ident) {
			g_client.ReleasePickup(ident);
		};
		pickups.Collected = [](const PickupIdent &ident) {
			g_client.CollectedPickup(ident);
		};
		pickups.Dropped = [](const PickupDropBody &drop) {
			return g_client.DroppedPickup(drop);
		};
		pickups.IsReplicatedPed = [](int32_t pedRef) {
			return g_client.IsReplicatedPed(pedRef);
		};
		// Wired is not connected. Without this the seam hides every pickup
		// in the world from the engine whenever the socket is down, which
		// includes every second before the first connect and forever for
		// anybody who installed the .asi without running a server.
		pickups.HaveSession = []() { return g_client.IsConnected(); };
		game::SetPickupCallbacks(pickups);
	}

	// Rampages - docs/roadmap.md §5.10, game/darkel.h.
	//
	// Four detours, on CDarkel::StartFrenzy, CDarkel::RegisterKillByPlayer,
	// CDarkel::RegisterCarBlownUpByPlayer and CDarkel::ReadStatus. Not fatal,
	// and the failures are different enough that darkel.cpp logs them one at
	// a time.
	//
	// What they buy, in one line: without them a KILLFRENZY pickup starts a
	// rampage on every machine - the pickup work already does that for free -
	// but each machine counts only its own player's kills and ends its own
	// rampage on its own arithmetic, so one player gets the reward while
	// another is told it failed.
	if (!game::InstallRampageHooks())
		Log("CoopIII: rampages are counted per machine, so a rampage can end "
		    "differently on different screens");

	{
		game::RampageCallbacks rampage;
		rampage.Started = [](const RampageStartBody &body) {
			g_client.RampageStarted(body);
		};
		rampage.Kill = [](uint16_t model, uint8_t weapon, bool headshot) {
			g_client.RampageKilled(model, weapon, headshot);
		};
		rampage.CarDestroyed = [](uint16_t model, const UnownedVehicleKey &key) {
			g_client.RampageCarDestroyed(model, key);
		};
		rampage.Ended = [](uint8_t outcome) { g_client.RampageEnded(outcome); };
		rampage.HaveSession = []() { return g_client.IsConnected(); };
		game::SetRampageCallbacks(rampage);
	}

	// What a dead pedestrian leaves on the pavement - docs/pickups.md 10.
	// Two detours on the only two functions in the game that make a pickup
	// main.scm did not.
	//
	// Separate from the exclusivity hook above and separately non-fatal,
	// because the two failures are different. Without these a ped drop is
	// one machine's own, which is exactly where this project was an hour
	// ago; and a remote player's death keeps putting a gun on our pavement
	// that their own machine does not have.
	if (!game::InstallPedDropHooks())
		Log("CoopIII: what a dead pedestrian drops stays on one machine");

	// Breakable street objects - docs/objects.md. One detour on
	// CObject::ObjectDamage, which is the only way anything in this build
	// breaks a lamp post, and one on CWorld::TriggerExplosion, which is what
	// keeps the first one quiet during a blast that every machine already
	// agrees about.
	//
	// Not fatal: without it a row of lamp posts one player mowed down is a
	// row of intact lamp posts on every other screen, which is exactly the
	// behaviour this replaces.
	if (!game::InstallObjectHooks())
		Log("CoopIII: breaking street objects stays local to each machine");

	game::AddObjectsToBridge(bridge);
	{
		game::ObjectCallbacks objects;
		objects.Broken = [](const ObjectBreakBody &body) {
			return g_client.ReportObjectBroken(body);
		};
		objects.Settled = [](const ObjectRestBody &body) {
			return g_client.ReportObjectSettled(body);
		};
		// The one thing object.cpp cannot work out for itself: a bullet
		// leaves no collision record, so "did our player do this or are we
		// replaying somebody else's trigger pull" is combat.cpp's answer and
		// nobody else's. docs/objects.md 5.
		objects.InReplayedShot = []() { return game::ReplayingRemoteShot(); };
		objects.IsReplicatedPed = [](int32_t pedRef) {
			return g_client.IsReplicatedPed(pedRef);
		};
		objects.IsReplicatedVehicle = [](int32_t vehRef) {
			return g_client.IsReplicatedVehicle(vehRef);
		};
		// Wired is not connected, the same distinction pickup.h paid for:
		// without this the detour would walk two pool lookups on every
		// collision in single player and then try to send into a dead socket.
		objects.HaveSession = []() { return g_client.IsConnected(); };
		objects.IsHost      = []() { return g_client.IsHost(); };
		game::SetObjectCallbacks(objects);
	}


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

	// And the minimap. A remote player is drawn the way the local one is, as
	// the rotating arrow that shows which way they are facing, out of a detour
	// on CRadar::DrawBlips itself (game/radar.h). Checks the function it is
	// about to hook is really DrawBlips first, and refuses loudly rather than
	// hooking over whatever else has patched it.
	game::InstallRadarArrows(g_client);

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
			// Same reason: the arrow draw reads the roster every frame the
			// radar is on screen, so the detour goes before the client does.
			game::RemoveRadarArrows();
			g_client.Stop();
			game::RemovePausePolicy();
			game::RemoveTrainClock();
			game::RemovePlaneClock();
			game::RemoveLightClock();
			game::RemoveLiftBridgeClock();
			// Before the frame hook, and before MinHook goes away. Removing
			// the combat detours ends every projectile CoopIII was animating
			// for somebody else, and that has to happen while the detour
			// keeping them from exploding is still installed.
			game::RemoveCombatHooks();
			game::RemoveAimPitchHook();
			// Same reason, and it also puts CVehicleModelInfo::ms_compsToUse
			// back to { -2, -2 }. Leaving a component override behind would
			// have the game fit it to the next car it creates by itself, for
			// the rest of the session, with CoopIII gone and nothing left to
			// explain it.
			game::RemoveVehicleHooks();
			// After Client::Stop, which has already destroyed every helicopter
			// replica. A replica left behind with the ProcessControl detour
			// gone would start chasing the local player.
			game::RemoveHeliHooks();
			game::RemoveHeliGunHook();
			// Before the population hooks: the ScanForThreats detour asks the
			// replica index whether a ped is one of ours.
			game::RemoveCheatHooks();
			// After Client::Stop, which has told it the session is over.
			game::RemoveMoneyHook();
			// After Client::Stop, which has already destroyed every replica.
			// The peds this machine hosts are left alone: they are the engine's
			// own pedestrians and go on being pedestrians without us.
			game::RemovePopulationHooks();
			// Before the frame hook, so the last thing the log says about
			// the sweep is what it cost. The inspector goes with it: a
			// dangling function pointer into an unloading DLL is a worse
			// crash than the one this guard exists to stop.
			game::RemoveWorldProcessGuard();
			game::SetMovingListEntityInspector(nullptr);
			game::RemoveFrameHook();
			HookShutdown();
		}
		LogClose();
		break;
	}
	return TRUE;
}
