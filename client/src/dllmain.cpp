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
#include "discordapp.h"
#include "game/carcam.h"
#include "game/carextras.h"
#include "game/cargun.h"
#include "game/chat.h"
#include "game/cheats.h"
#include "game/combat.h"
#include "game/copcrime.h"
#include "game/cutscenehead.h"
#include "game/cutsceneskip.h"
#include "game/darkel.h"
#include "game/carremoval.h"
#include "game/carletgo.h"
#include "game/crowdfade.h"
#include "game/crowdrange.h"
#include "game/emergency.h"
#include "game/fontcull.h"
#include "game/frame.h"
#include "game/garage.h"
#include "game/gates.h"
#include "game/heli.h"
#include "game/heligun.h"
#include "game/liftbridge.h"
#include "game/radio.h"
#include "game/lights.h"
#include "game/mine.h"
#include "game/mission.h"
#include "game/money.h"
#include "game/nametag.h"
#include "game/newgame.h"
#include "game/object.h"
#include "game/glass.h"
#include "game/passengeraim.h"
#include "game/passexit.h"
#include "game/pause.h"
#include "game/ped.h"
#include "game/pickup.h"
#include "game/planes.h"
#include "game/population.h"
#include "game/radar.h"
#include "game/rampagevote.h"
#include "game/tpto.h"
#include "game/ridecam.h"
#include "game/runover.h"
#include "game/bump.h"
#include "game/animcb.h"
#include "game/social.h"
#include "game/replicacalm.h"
#include "game/stunt.h"
#include "game/scoreboard.h"
#include "game/seat.h"
#include "game/sessionclock.h"
#include "game/sidejob.h"
#include "game/skin.h"
#include "game/standapart.h"
#include "game/trains.h"
#include "game/vehicle.h"
#include "game/wanted.h"
#include "game/verify.h"
#include "game/world.h"
#include "game/worldstate.h"
#include "hook/hook.h"
#include "log.h"
#include "presence.h"

#include <coopiii/mission.h>
#include <coopiii/version.h>

#include <cstdlib>
#include <ctime>

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

void InstallOnGameThread();
bool g_installedAll = false;

// Discord Rich Presence (presence.h): on once Boot has decided CoopIII runs,
// and handed what to show from PostFrame, once a second.
bool              g_presenceOn = false;
presence::Tracker g_presence;
uint32_t          g_presenceNextMs = 0;

void TickPresence() {
	const uint32_t now = WallClock::NowMs();
	if (!g_presenceOn || static_cast<int32_t>(now - g_presenceNextMs) < 0)
		return;
	g_presenceNextMs = now + 1000;

	const bool         connected = g_client.IsConnected();
	const MissionSync &missions  = g_client.Missions();
	const bool         running   = connected && missions.Running();
	// The title the game shows for it, out of the mission table.
	const char *title = running && missions.Number() < MISSION_COUNT
	                        ? MissionName(missions.Number())
	                        : nullptr;
	presence::Update(g_presence.Observe(connected, running, title,
	                                    static_cast<uint8_t>(g_client.RemoteCount() + 1),
	                                    g_client.SessionSlots(),
	                                    static_cast<int64_t>(std::time(nullptr))));
}

void PreFrame() {
	// The rest of the install, once, before this frame touches anything it
	// sets up.
	if (!g_installedAll) {
		g_installedAll = true;
		InstallOnGameThread();
	}

	// Before anything else this frame, before CGame::Process even decides
	// whether to update the world. game/pause.h has the full reasoning.
	game::ClearPauseForTheWorld();
	g_client.PreFrame();
	game::TickSocial();
	game::TickChat();
	// Once, on the first frame: here because no glyph is being printed
	// (game/fontcull.cpp).
	game::FixFontCull();
	// A participant's $ONMISSION, held before this frame's scripts run.
	game::TickMissions();
	// A passenger the exit key did not get out (game/passexit.h).
	game::TickPassengerExit(WallClock::NowMs());

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

	// The vote before a rampage: the help box, Y and N, and a move to the
	// player who touched the skull. Before CGame::Process, so a move to
	// another island is seen by this frame's CCollision::Update.
	// CoopIII's own typed cheats first: TPTO starts the same move, which the
	// vote's tick then carries on (game/tpto.h).
	game::TickCoopCheats();
	game::TickRampageVote();

	// Two players on one spot: the one who does not keep it moves off it
	// (game/standapart.h). After the vote's move, which may have just put
	// us down.
	game::TickStandApart();

	// Skipping a cutscene together: what this game is in, for the server, and
	// a skip the others agreed on. Before CGame::Process, whose
	// CCutsceneMgr::Update is where the skip input is read.
	game::TickCutsceneSkip();
}

void PostFrame() {
	g_client.PostFrame();

	// A game that loses its display keeps running only in a session
	// (game/pause.h); read here, so the next frame it skips sees it.
	game::SetSessionForUnfocusedFrames(g_client.IsConnected());

	// The session's mission's blue markers, drawn after this frame's scripts
	// the way the owner's own checks draw them, and before the frame is.
	game::DrawMissionMarkers();

	// After CGame::Process, which is exactly where CPhysical::ProcessControl
	// has just finished deciding whether each loose object is asleep. Walks
	// the handful of objects this machine knocked over and owes everybody a
	// resting place for, and is a no-op on every frame nobody hit anything.
	game::TickUprootedObjects();

	// Last, so the flag agrees with the menu again by the time
	// cAudioManager::Service and the HUD read it - both run after
	// CGame::Process returns.
	game::RestorePauseForPresentation();

	// What Discord shows; a copy under a lock, once a second.
	TickPresence();

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

		// Glass, on the same rule. `shattered here` moving while `reported`
		// stays at zero is a game with no session to tell; `left to the
		// shooter` is somebody else's round on a cracked window, whose roll
		// is theirs and arrives as `from the wire` when it shatters.
		const game::GlassStats &g = game::GetGlassStats();
		if (g.shatteredHere || g.received || g.latched || g.leftToShooter)
			Log("frame: glass - %u shattered here, %u reported, %u left to the "
			    "shooter, %u from the wire (%u applied, %u had nothing here, %u "
			    "already broken, %u refused), %u latched from the session's record",
			    g.shatteredHere, g.reported, g.leftToShooter, g.received, g.applied,
			    g.receivedUnmatched, g.receivedNoop, g.refused, g.latched);

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

		// Cutscene heads held for want of an animation (game/cutscenehead.h),
		// only once there has been one.
		if (const uint32_t held = game::CutsceneHeadsHeld())
			Log("frame: cutscene heads - %u frame(s) held with no animation", held);
	}
}

DWORD WINAPI Boot(LPVOID) {
	WallClock::Start();

	g_config.LoadFromFile(Config::IniPath());
	g_config.ApplyEnvOverrides();
	if (g_config.logToFile)
		LogOpen(Config::PathNextToModule("CoopIII.log"));

	Log("CoopIII " COOPIII_VERSION " starting (pid %lu, protocol %u)", GetCurrentProcessId(),
	    static_cast<unsigned>(PROTOCOL_VERSION));

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
	Log("config: server %s:%u, nick \"%s\"%s (from %s)", g_config.host.c_str(), g_config.port,
	    g_config.nick.c_str(), g_config.password.empty() ? "" : ", with a password",
	    Config::IniPath().c_str());

	const game::VerifyResult image = game::VerifyGameImage();
	Log("image: %s", image.detail.c_str());
	if (!image.ok) {
		Log("CoopIII will not load. No hooks were installed and the game is "
		    "untouched.");
		return 0;
	}

	// "Playing CoopIII" in Discord rather than the GTA III it finds by itself.
	// Its own thread, and nothing at all without Discord running.
	if (g_config.discordPresence) {
		const uint64_t appId = g_config.discordAppId != 0 ? g_config.discordAppId : DISCORD_APP_ID;
		if (appId != 0) {
			presence::Update(g_presence.Observe(false, false, nullptr, 0, 0,
			                                    static_cast<int64_t>(std::time(nullptr))));
			presence::Start(appId);
			g_presenceOn = true;
		}
	} else {
		Log("discord: discordPresence is off, so Discord shows the game as it finds it");
	}

	// The lobby's host started everybody's game into a new game
	// (docs/protocol.md 1.31): once the menu is up, it starts one the way its
	// New Game does. Hooked before the game runs, the one thing that is, since
	// the game only runs once somebody has left the menu.
	if (const char *fresh = std::getenv("COOPIII_NEW_GAME"); fresh && fresh[0] == '1') {
		if (HookInit() && game::ArmNewGameFromMenu())
			Log("newgame: the lobby's host started a new game for everybody, and it starts "
			    "by itself once the menu is up");
		else
			Log("newgame: the lobby's host started a new game for everybody, but the menu "
			    "could not be hooked, so pick New Game yourself");
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
	return 0;
}

// Everything past the frame hook, run once from the first PreFrame. On the
// game thread on purpose: the call-site redirects and the horn stub rewrite
// code the game thread runs every frame, and nothing suspends it for a plain
// memcpy the way MinHook does for a detour, so from the boot thread a frame
// could fetch a half-written jump. Client::Start and several installs also
// call into the engine and reset state PreFrame reads, which from another
// thread raced the frame they landed in.
void InstallOnGameThread() {
	// The seatbelt on CWorld::Process, and the inspector it hands every
	// surviving entity to. Installed straight after the frame hook and
	// before anything that can create or destroy an entity, because the one
	// thing it must not do is start late.
	//
	// Not fatal: without it the sweep falls back to PreFrame, which is where
	// it was when it failed to catch three crashes in a row (game/world.h).
	game::SetMovingListEntityInspector(&game::ClampClumpAnimations);
	game::InstallWorldProcessGuard();

	// A cutscene head the world walks before SET_HEAD_ANIM has given it an
	// animation reads through a null one (game/cutscenehead.h). Three call
	// sites, no detour, and no session needed: a head with no .anm is a
	// single-player crash too. Not fatal.
	game::InstallCutsceneHeadGuard();

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

	// A car another player drives, hitting us: priced by its speed, and
	// friendly fire decides whether it costs health. Not fatal.
	game::InstallRunOverHooks();

	// Our car into another machine's: what the collision did to the copy is
	// seen and sent to its owner (game/bump.h). Not fatal.
	game::InstallBumpWatch();

	// The two car animation callbacks that read the car with no test, handed
	// to the engine through a wrapper that has one (game/animcb.h). Not fatal:
	// every seat CoopIII changes by hand takes those callbacks off first.
	game::InstallCarCallbackGuards();

	// Whom the lock-on picks, what a respawn clears, whose honk scatters a
	// crowd and whose foot the engine sound follows (game/social.h). Not
	// fatal: whatever does not install is the engine's own behaviour.
	game::InstallSocialHooks(g_client);
	// A remote player's copy neither side-steps when bumped nor, as a
	// passenger, looks round at passers-by. Cosmetic, not fatal.
	game::InstallReplicaCalm();

	// The stunt threads' question about a car in the air, answered no for a
	// car somebody else's engine moves. Not fatal.
	game::InstallStuntGuard();

	// The odd jobs' key, heard from the wheel only (game/sidejob.h). Not fatal.
	game::InstallSideJobKey();

	// A remote player's arms up or down with their aim. Cosmetic: the shot
	// itself is aimed off C_Shot's direction whether this installs or not.
	game::InstallAimPitchHook();
	game::InstallLegTwist();

	// Which clothes a remote Claude has on. Cosmetic as well: without it they
	// all wear whatever our model 0 is, which is how it always was.
	game::InstallLookHook();

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
	// Reads the replica index, so after it. Not fatal: without it somebody
	// else's policeman is a civilian to our wanted level.
	game::InstallCopCrimeHook();
	// Medics and fire trucks (game/emergency.h). Reads the replica index and
	// the car tables, so after both. Not fatal: whatever is not taken stays
	// each machine's own, as it always was.
	game::InstallEmergencyHooks();
	// The tank's cannon and the fire truck's water cannon answer to the
	// driver's pad only, never a passenger's (game/cargun.h). Two call sites,
	// no detour. Not fatal: without them a passenger works the tank's gun as
	// in single player, though the emergency hooks above still refuse his
	// jet from a fire truck somebody else drives.
	game::InstallCarGunGate();
	// A player riding as a passenger gets out by his own seat's door, not
	// round the car at the driver's, and keeps his seat when another player
	// takes the wheel (game/passexit.h). Two call sites and the room test
	// inside SetExitCar, no detour. Not fatal: without them the engine has
	// its way, as in single player.
	game::InstallPassengerExit();
	// The crusher, the crane and the garages (game/carremoval.h). Reads the
	// car tables, so after them. Not fatal: whatever is not taken goes on
	// acting on every machine's own copy, as it always did.
	game::InstallCarRemovalHooks();
	// Other machines' traffic kept out of a wanted player's police count
	// (game/crowdrange.h). Not fatal: without it he may see few police cars.
	game::InstallCrowdRange();
	// A car of ours our engine drops beside another player is handed to him
	// (game/carletgo.h). Not fatal: without it such a car is despawned in
	// front of him, as it always was.
	game::InstallCarLetGo();

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
	// The car radio (game/radio.h): three calls in the music manager pointed
	// at us, none fatal. Without them every copy of a car plays its own
	// station from wherever its own machine left it, as it always has.
	game::InstallRadioSync();

	// The police helicopter (game/heli.h). Six detours, none fatal: without
	// them each wanted player's helicopter is his machine's alone, which is
	// how it has always been. Before MakeWorldBridge's callers below, because
	// AddHeliToBridge only offers replicas once the ProcessControl detour is
	// in - a replica without it would run the real pilot logic against this
	// machine's player.
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
	game::AddRadioToBridge(bridge);
	game::AddCarExtrasToBridge(bridge);
	game::AddVehicleBlastToBridge(bridge);
	game::SetSeatKey(g_config.seatKey);
	game::AddSeatToBridge(bridge);
	game::AddRideCameraToBridge(bridge);
	game::AddPopulationToBridge(bridge);
	game::AddEmergencyToBridge(bridge);
	game::AddCarRemovalToBridge(bridge);
	game::AddCrowdRangeToBridge(bridge);
	// Nothing synced appearing or vanishing in front of anybody
	// (game/crowdfade.h): our camera, and our copies faded in and out.
	game::AddCrowdFadeToBridge(bridge);
	// The wanted level. Two reads and one write into the local player's own
	// CWanted, and nothing else: the police are ambient entities that
	// AddPopulationToBridge above has been replicating all along
	// (docs/wanted.md §4.2).
	game::InstallWantedBridge(bridge);
	// Rampages. Four entries and three detours; game/darkel.h is the design.
	game::InstallRampageBridge(bridge);
	game::SetRampageVoteKeys(g_config.voteYesKey, g_config.voteNoKey);
	game::AddHeliToBridge(bridge);
	game::AddHeliGunToBridge(bridge);
	game::AddCheatsToBridge(bridge);
	game::AddMoneyToBridge(bridge);
	// Each remote player in his own custom skin, and ours to them (game/skin.h).
	// No hook: a remote ped's atomics are given a render callback of their own.
	game::AddSkinsToBridge(bridge);
	// The session's one mission (game/mission.h). Seven detours on the script
	// engine's range handlers, and only with `missions = on`: the addresses are
	// III.CLEO's and plugin-sdk's, not yet this project's own proof. Without it
	// every mission stays this machine's own, as it always has.
	game::InstallMissionHooks(g_config.missions, g_client);
	game::AddMissionsToBridge(bridge);
	game::SetChatKeys(g_config.chatKey, g_config.listKey);
	game::SetScoreboardKey(g_config.scoreboardKey);
	game::SetScoreboardServer(g_config.host, g_config.port);
	game::SetVersionMarkShown(g_config.showVersion);
	game::AddChatToBridge(bridge);
	game::AddSocialToBridge(bridge);
	// Whether our window is in front, for C_PlayerAway (game/pause.h).
	bridge.LocalWindowInFront = &game::GameWindowInFront;
	// Which island a remote player stands on, against the one loaded here
	// (game/nametag.h, RemoteOnOtherIsland).
	bridge.IslandAt     = &game::IslandAt;
	bridge.IslandLoaded = &game::IslandLoaded;

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
	// And main.scm's seven gates, the same union (game/gates.h). No detour of
	// its own: it reads SLIDE_OBJECT through the mission hooks.
	game::AddGatesToBridge(bridge);

	// The mines a mission drops go off on every machine, whichever sees it
	// first. Not fatal: without it each machine's mines are its own.
	if (!game::InstallMineHooks())
		Log("CoopIII: mines go off on each machine by itself");
	game::AddMinesToBridge(bridge);

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
		rampage.Kill = [](uint16_t model, uint8_t weapon, bool headshot, uint8_t killer,
		                  uint8_t pedType) {
			g_client.RampageKilled(model, weapon, headshot, killer, pedType);
		};
		rampage.CarDestroyed = [](uint16_t model, const UnownedVehicleKey &key) {
			g_client.RampageCarDestroyed(model, key);
		};
		rampage.Ended = [](uint8_t outcome) { g_client.RampageEnded(outcome); };
		rampage.RewardPaid = [](int32_t amount) { g_client.RampageRewardPaid(amount); };
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
		objects.Rebuilt     = [](const ObjectIdent &ident) { g_client.ReportObjectRebuilt(ident); };
		game::SetObjectCallbacks(objects);
	}

	// Shattered glass, docs/objects.md 10. The four calls that shatter a
	// window, taken at the call. Not fatal: a site not taken leaves its cause
	// breaking windows on one screen, and the log names it.
	if (!game::InstallGlassHooks())
		Log("CoopIII: some windows shatter on one machine only (the lines above say which)");
	game::AddGlassToBridge(bridge);
	{
		game::GlassCallbacks glass;
		glass.Shattered = [](const GlassBreakBody &body) {
			return g_client.ReportGlassBroken(body);
		};
		glass.InSomebodyElsesRound = []() {
			return game::ReplayingRemoteShot() || game::DrawingRemoteRoundOnGlass();
		};
		glass.HaveSession = []() { return g_client.IsConnected(); };
		game::SetGlassCallbacks(glass);
	}


	g_client.SetPassword(g_config.password);
	if (!g_client.Start(g_config.host, g_config.port, g_config.nick, bridge)) {
		Log("CoopIII: the network client failed to start; the frame hook stays "
		    "installed but nothing will be sent");
		return;
	}

	// Nametags hang off CHud::Draw rather than the frame pump, because they're
	// drawn in the render pass and everything the frame pump reaches happens
	// before it (game/nametag.cpp). Last, and only once there's a session for
	// them to label, so a client that never starts doesn't leave a detour on
	// the HUD with nothing to draw. Not fatal if it fails: the session still
	// works, you just can't tell who is who.
	game::SetNametagScale(g_config.nametagScale);
	game::InstallNametags(g_client);
	// Drawn from the same detour, so without it the chat still goes out and
	// comes in (the server log has it) but nobody sees it.
	game::InstallChat(g_client);

	// And the minimap. A remote player is drawn the way the local one is, as
	// the rotating arrow that shows which way they are facing, out of a detour
	// on CRadar::DrawBlips itself (game/radar.h). Checks the function it is
	// about to hook is really DrawBlips first, and refuses loudly rather than
	// hooking over whatever else has patched it.
	game::InstallRadarArrows(g_client);

	// The vote before a rampage. Nothing hooked; ticked from PreFrame.
	game::InstallRampageVote(g_client);
	// CoopIII's own typed cheats, TPTO1..TPTO8. Nothing hooked; the keys are
	// read off the chat's window procedure and ticked from PreFrame.
	game::InstallTpto(g_client);

	// Two players put on one spot. Nothing hooked; ticked from PreFrame.
	game::InstallStandApart(g_client);

	// Skipping a cutscene together. Two call sites, no detour: the skip input's
	// call to FinishCutscene and the intro's button test (game/cutsceneskip.h).
	// Not fatal: without the first every skip stays this machine's own.
	game::InstallCutsceneSkip(g_client);

	// A rider's camera: the car he rides in corrected before the frame's
	// camera follows it, and the driver's unique jump shot on his screen
	// (game/ridecam.h). Three call sites, no detour. Not fatal.
	game::InstallRideCamera(g_client);

	// A passenger's gun: the on-foot mouse camera around him with its four
	// collision calls taken so it ignores his car, and his rounds through
	// CWeapon::Fire (game/passengeraim.h). Ticked from the camera call above,
	// so nothing without it. Not fatal.
	if (game::RideCameraOrderTaken())
		game::InstallPassengerAim();

	// A car camera mod such as SACarCam: read and logged, and when one owns
	// the car camera's call, chained so it keeps off the gun of a car
	// somebody else drives (game/carcam.h). One call site, no detour. Not
	// fatal.
	game::InstallCarCamera();

	Log("CoopIII ready");
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
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
		// A non-null `reserved` is the process exiting, not an unload. By then
		// the game has run CGame::ShutDown - the pools are freed, CWorld and
		// RenderWare are gone - and every other thread has been killed
		// wherever it stood. The teardown below would destroy remote peds and
		// cars through freed pools and take locks a dead thread may hold, all
		// to tidy a process that is about to vanish. Only an explicit unload
		// runs it.
		if (reserved != nullptr)
			break;
		// The pipe first: its thread reads nothing of the game, but it is ours.
		presence::Stop();
		if (g_started) {
			// First, because the draw reads the roster straight out of the
			// client and the client is about to be stopped.
			game::RemoveNametags();
			// And the window procedure, which reads the client's session state.
			game::RemoveChat();
			// Same reason: the arrow draw reads the roster every frame the
			// radar is on screen, so the detour goes before the client does.
			game::RemoveRadarArrows();
			game::RemoveRampageVote();
			game::RemoveTpto();
			game::RemoveStandApart();
			game::RemoveCutsceneSkip();
			game::RemoveCarCamera();
			game::RemovePassengerAim();
			game::RemoveRideCamera();
			g_client.Stop();
			game::RemovePausePolicy();
			game::RemoveTrainClock();
			game::RemovePlaneClock();
			game::RemoveLightClock();
			game::RemoveLiftBridgeClock();
			game::RemoveRadioSync();
			// Before the frame hook, and before MinHook goes away. Removing
			// the combat detours ends every projectile CoopIII was animating
			// for somebody else, and that has to happen while the detour
			// keeping them from exploding is still installed.
			game::RemoveRunOverHooks();
			game::RemoveBumpWatch();
			game::RemoveCarCallbackGuards();
			game::RemoveSocialHooks();
			game::RemoveReplicaCalm();
			game::RemoveMineHooks();
			game::RemoveStuntGuard();
			game::RemoveSideJobKey();
			game::RemoveCombatHooks();
			game::RemoveAimPitchHook();
			game::RemoveLegTwist();
			game::RemoveLookHook();
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
			game::RemoveCopCrimeHook();
			game::RemoveEmergencyHooks();
			game::RemoveCarGunGate();
			game::RemovePassengerExit();
			game::RemoveCarRemovalHooks();
			game::RemoveCrowdRange();
			game::RemoveCarLetGo();
			game::RemoveMissionHooks();
			// The object hooks own a call-site redirect as well as detours,
			// and a redirect has no destructor to put it back: left in place
			// it sends the population manager into an unloaded module.
			game::RemoveObjectHooks();
			// Four redirects, for the same reason.
			game::RemoveGlassHooks();
			game::RemovePickupHook();
			game::RemovePedDropHooks();
			game::RemoveRampageHooks();
			// After Client::Stop, which has already destroyed every replica.
			// The peds this machine hosts are left alone: they are the engine's
			// own pedestrians and go on being pedestrians without us.
			game::RemovePopulationHooks();
			// Before the frame hook, so the last thing the log says about
			// the sweep is what it cost. The inspector goes with it: a
			// dangling function pointer into an unloading DLL is a worse
			// crash than the one this guard exists to stop.
			game::RemoveWorldProcessGuard();
			game::RemoveCutsceneHeadGuard();
			game::SetMovingListEntityInspector(nullptr);
			game::RemoveFrameHook();
			HookShutdown();
		}
		LogClose();
		break;
	}
	return TRUE;
}
