#include "rampagevote.h"

#include "addresses.h"
#include "ped.h"
#include "pedanim.h"
#include "../client.h"
#include "../clock.h"
#include "../log.h"
#include "../rampagevoteview.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace coopiii::game {

namespace {

static_assert(sizeof(wchar_t) == 2, "the game's text is 16-bit");

Client *g_client = nullptr;
int     g_yesKey = 'Y';
int     g_noKey  = 'N';
bool    g_yesWasDown = false;
bool    g_noWasDown  = false;

void *PlayerPed() {
	return Func<void *(__cdecl *)()>(FindPlayerPed)();
}

uint8_t *LocalPlayerInfo() {
	const uint8_t focus = Global<uint8_t>(CWorld__PlayerInFocus);
	return reinterpret_cast<uint8_t *>(CWorld__Players + focus * offs::PLAYERINFO_STRIDE);
}

// ---------------------------------------------------------------------------
// The help box
// ---------------------------------------------------------------------------

wchar_t *HelpText() { return Ptr<wchar_t>(CHud__m_HelpMessage); }
int32_t &HelpState() { return Global<int32_t>(CHud__m_HelpMessageState); }

// The line we last put in the box, so ours can be told from the game's.
wchar_t g_ours[HUD_HELP_LEN] = {};
bool    g_haveOurs = false;

bool SameText(const wchar_t *a, const wchar_t *b) {
	return std::wcsncmp(a, b, HUD_HELP_LEN) == 0;
}

bool BoxShowsOurs() {
	return g_haveOurs && SameText(HelpText(), g_ours);
}

// Something is up there that isn't ours: a mission's line, a cheat's, the
// game's own hints. State 0 is an empty box whatever the text says - the fade
// out leaves the last text where it was.
bool BoxTakenByTheGame() {
	return HelpState() != HUD_HELP_STATE_NONE && !BoxShowsOurs();
}

// The game's own way in, with its fade and its beep.
void ShowFresh(const wchar_t *text) {
	std::wcsncpy(g_ours, text, HUD_HELP_LEN - 1);
	g_ours[HUD_HELP_LEN - 1] = L'\0';
	g_haveOurs = true;
	using SetFn = void(__cdecl *)(const wchar_t *, bool);
	Func<SetFn>(CHud__SetHelpMessage)(g_ours, false);
}

// A new count or a new second, into the line that is already up. Only once
// CHud::Draw has taken it (state 1 or 2): before that the last-shown buffer
// is still zero, which is what makes Draw start the fade, and filling it in
// would leave the box empty.
void KeepOursUp(const wchar_t *text) {
	const int32_t state = HelpState();
	if (state != HUD_HELP_STATE_SHOWN && state != HUD_HELP_STATE_FADE_IN)
		return;
	if (!SameText(text, g_ours)) {
		std::wcsncpy(g_ours, text, HUD_HELP_LEN - 1);
		g_ours[HUD_HELP_LEN - 1] = L'\0';
		std::memcpy(Ptr<wchar_t>(CHud__m_HelpMessage), g_ours, sizeof g_ours);
		std::memcpy(Ptr<wchar_t>(CHud__m_LastHelpMessage), g_ours, sizeof g_ours);
		std::memcpy(Ptr<wchar_t>(CHud__m_HelpMessageToPrint), g_ours, sizeof g_ours);
	}
	// Held at the start of its display time for as long as the vote runs.
	if (state == HUD_HELP_STATE_SHOWN)
		Global<int32_t>(CHud__m_HelpMessageTimer) = 0;
}

// Which vote the box is about, and what is left to say about it.
uint8_t  g_voteOnScreen = 0;      // the open vote we are showing or waiting to show
bool     g_saidShown    = false;
bool     g_saidYielded  = false;
uint32_t g_ticksQuiet   = 0;      // ticks our line has sat in an empty box
uint8_t  g_resultFor    = 0;      // the vote whose result has been dealt with
bool     g_resultWaiting = false;
uint8_t  g_resultState   = 0;
uint8_t  g_resultVote    = 0;
uint32_t g_resultUntilMs = 0;

// How long a result waits for a busy box before it is dropped.
constexpr uint32_t RESULT_WAIT_MS = 5000;

void TickHelpBox(const RampageVoteView &v, uint32_t now) {
	if (v.Open()) {
		if (g_voteOnScreen != v.body.voteId) {
			g_voteOnScreen = v.body.voteId;
			g_saidShown    = false;
			g_saidYielded  = false;
			g_haveOurs     = false;
		}
		wchar_t text[HUD_HELP_LEN];
		const char *nick = g_client->NickFor(v.body.starterId);
		FormatRampageVote(text, HUD_HELP_LEN, nick, g_yesKey, g_noKey, v.body.yes,
		                  v.body.voters, v.SecondsLeft(now));

		// Ours, but the box has gone quiet with it long after it was put up:
		// it was allowed to fade (a loading screen skips this tick, and the
		// timer ran). Up again, the game's way.
		const bool faded = BoxShowsOurs() && HelpState() == HUD_HELP_STATE_NONE &&
		                   ++g_ticksQuiet > 2;
		if (BoxShowsOurs() && HelpState() != HUD_HELP_STATE_NONE)
			g_ticksQuiet = 0;

		if (BoxShowsOurs() && !faded) {
			KeepOursUp(text);
		} else if (!BoxTakenByTheGame()) {
			g_ticksQuiet = 0;
			ShowFresh(text);
			if (!g_saidShown) {
				g_saidShown = true;
				Log("rampage vote %u: on screen, %s wants a rampage", v.body.voteId,
				    nick ? nick : "?");
			}
		} else if (g_haveOurs && !g_saidYielded) {
			// The game took the box while ours was up. Its line wins; ours
			// comes back when it's gone, if the vote is still running.
			g_saidYielded = true;
			Log("rampage vote %u: the game put its own help line up, ours waits",
			    v.body.voteId);
		}
		return;
	}

	// It has ended. Once per vote - including one that opened and ended
	// between two frames, which never got a line of its own.
	if (!v.seen) {
		// No session, or a new one: whatever was up belongs to the old one.
		g_voteOnScreen = 0;
		g_resultFor    = 0;
		g_haveOurs     = false;
	} else if (v.body.voteId != g_resultFor && now - v.atMs < RESULT_WAIT_MS) {
		g_resultFor     = v.body.voteId;
		g_resultWaiting = true;
		g_resultState   = v.body.state;
		g_resultVote    = v.body.voteId;
		g_resultUntilMs = now + RESULT_WAIT_MS;
		g_voteOnScreen  = 0;
	}

	if (!g_resultWaiting)
		return;
	if (BoxTakenByTheGame()) {
		if (static_cast<int32_t>(now - g_resultUntilMs) >= 0) {
			g_resultWaiting = false;
			g_haveOurs      = false;
			Log("rampage vote %u: the help box stayed busy, the result wasn't shown",
			    g_resultVote);
		}
		return;
	}
	wchar_t text[HUD_HELP_LEN];
	RampageWiden(text, HUD_HELP_LEN, RampageVoteResultText(g_resultState));
	ShowFresh(text);
	// From here it's an ordinary help line and the game fades it out.
	g_haveOurs      = false;
	g_resultWaiting = false;
	Log("rampage vote %u: showed \"%s\"", g_resultVote, RampageVoteResultText(g_resultState));
}

// ---------------------------------------------------------------------------
// The keys
// ---------------------------------------------------------------------------

bool GameHasTheKeyboard() {
	const HWND fg    = GetForegroundWindow();
	DWORD      owner = 0;
	if (fg)
		GetWindowThreadProcessId(fg, &owner);
	return owner == GetCurrentProcessId();
}

// The chat line holds the controls with CoopIII's own bit, and so does the
// menu (game/pause.h), so one test answers both. The menu flag too, for a
// build where the pause policy didn't install.
bool KeysAreSomebodyElses() {
	if (Global<uint8_t>(CMenuManager__m_bMenuActive) != 0)
		return true;
	const uint8_t controls = Global<uint8_t>(CPad__Pads + pad::DISABLE_PLAYER_CONTROLS);
	return (controls & pad::PLAYERCONTROL_COOPIII) != 0;
}

void TickKeys(const RampageVoteView &v) {
	const bool yesDown = (GetAsyncKeyState(g_yesKey) & 0x8000) != 0;
	const bool noDown  = (GetAsyncKeyState(g_noKey) & 0x8000) != 0;
	const bool yesEdge = yesDown && !g_yesWasDown;
	const bool noEdge  = noDown && !g_noWasDown;
	g_yesWasDown = yesDown;
	g_noWasDown  = noDown;

	if (!yesEdge && !noEdge)
		return;
	if (!v.MayCast(g_client->LocalPlayerId()) || PlayerPed() == nullptr)
		return;
	if (!GameHasTheKeyboard() || KeysAreSomebodyElses())
		return;
	// Both in the same frame is nobody's answer.
	if (yesEdge && noEdge)
		return;
	g_client->CastRampageVote(yesEdge);
}

// ---------------------------------------------------------------------------
// The move
// ---------------------------------------------------------------------------

struct Move {
	bool                active = false;
	// A rampage vote's, which the server hears the end of; otherwise the
	// session's mission bringing a participant to its owner (MovePlayerBeside).
	bool                vote   = true;
	// What the log calls a move that is not a vote's: "missions", or "tpto"
	// for the cheat (game/tpto.h).
	const char         *tag    = "missions";
	RampageTeleportBody body{};
	int32_t             level  = 0;
	bool                locked = false;
	uint32_t            frames = 0;
	float               hold[3] = {};
};
Move g_move;

// What the log calls the move.
const char *Tag() { return g_move.vote ? "rampage" : g_move.tag; }

// Give up waiting for an island's collision after this long, and put him
// down where he is being held. Loading an island takes a second or two with
// its loading screen; ten is a fault.
constexpr uint32_t COLLISION_WAIT_FRAMES = 600;

bool CutsceneRunning() {
	return Global<uint8_t>(CCutsceneMgr__ms_running) != 0 ||
	       Global<uint8_t>(CCutsceneMgr__ms_cutsceneProcessing) != 0 ||
	       (Global<uint8_t>(CPad__Pads + pad::DISABLE_PLAYER_CONTROLS) &
	        pad::PLAYERCONTROL_CUTSCENE) != 0;
}

TeleportFacts ReadFacts(void *ped) {
	TeleportFacts f;
	f.havePed = ped != nullptr;
	if (ped) {
		f.health   = Field<float>(ped, offs::PED_HEALTH);
		f.pedState = Field<uint32_t>(ped, offs::PED_STATE);
	}
	f.wbState  = *(LocalPlayerInfo() + offs::PLAYERINFO_WB_STATE);
	f.cutscene = CutsceneRunning();
	using BoolFn    = bool(__cdecl *)();
	f.onMission     = Func<BoolFn>(CTheScripts__IsPlayerOnAMission)();
	f.frenzyOngoing = Func<BoolFn>(CDarkel__FrenzyOnGoing)();
	return f;
}

const char *WhyNot(uint8_t verdict) {
	switch (verdict) {
	case RAMPAGE_SKIPPED_DEAD:     return "dead";
	case RAMPAGE_SKIPPED_ARRESTED: return "being arrested";
	case RAMPAGE_SKIPPED_CUTSCENE: return "in a cutscene";
	case RAMPAGE_SKIPPED_MISSION:  return "on a mission";
	case RAMPAGE_SKIPPED_NO_PED:   return "no player in the world";
	default:                       return "?";
	}
}

// Out of whatever car he is in, the way COMMAND_WARP_CHAR_FROM_CAR_TO_COORD's
// handler does it (addresses.h, "getting out" and "moving the local
// player"), up to the teleport, which the caller does. A player halfway into
// a car has the entry called off instead.
void OutOfAnyCar(void *ped) {
	const uint32_t state    = Field<uint32_t>(ped, offs::PED_STATE);
	const bool     inCar    = Field<bool>(ped, offs::PED_IN_VEHICLE);
	const bool     entering = state == PEDSTATE_ENTER_CAR || state == PEDSTATE_CARJACK;
	if (!inCar && !entering)
		return;

	if (entering && !inCar) {
		CancelCarEntry(ped);
		Log("%s: called off getting into a car before the move", Tag());
	}

	if (inCar) {
		if (void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE)) {
			// Halfway out: the door that exit claimed goes back first.
			ReleaseExitDoor(ped, car);
			if (Field<uint8_t>(car, offs::VEH_FLAGS_B_BUS) & offs::VEH_IS_BUS)
				Field<uint8_t>(ped, offs::PED_FLAGS_C) |= offs::PED_RENDER_IN_CAR;
			if (Field<void *>(car, offs::VEH_DRIVER) == ped) {
				// RemoveDriver is SetStatus(ABANDONED) and pDriver = nil.
				Func<void(__thiscall *)(void *)>(CVehicle__RemoveDriver)(car);
				Field<uint8_t>(car, offs::VEH_FLAGS_A) &= static_cast<uint8_t>(~offs::VEH_ENGINE_ON);
				Field<uint8_t>(car, offs::AUTOPILOT_CRUISE_SPEED) = 0;
			} else {
				Func<void(__thiscall *)(void *, void *)>(CVehicle__RemovePassenger)(car, ped);
			}
			float *const mv = &Field<float>(car, offs::MOVE_SPEED);
			mv[0] = 0.0f;
			mv[1] = 0.0f;
			mv[2] = VEH_EXIT_SETTLE_SPEED_Z;
			float *const tv = &Field<float>(car, offs::TURN_SPEED);
			tv[0] = tv[1] = tv[2] = 0.0f;
		}
		Log("%s: out of the car for the move", Tag());
	}

	// And the ped, the rest of the handler's way (ped.h, PutPedOnFoot), which
	// the seat key's own way out now shares.
	PutPedOnFoot(ped, inCar);
}

void TeleportPed(void *ped, float x, float y, float z) {
	// Through the vtable, slot 11, the way the warp handler calls it
	// (`call [ebx+2Ch]` at 0x0044B505). The CVector goes by value, three
	// floats on the stack. For a player that's CPed::Teleport (0x004D3E70).
	using TeleportFn = void(__thiscall *)(void *, float, float, float);
	void *const *vtable = *reinterpret_cast<void *const *const *>(ped);
	reinterpret_cast<TeleportFn>(vtable[11])(ped, x, y, z);
	float *const vel = &Field<float>(ped, offs::MOVE_SPEED);
	vel[0] = vel[1] = vel[2] = 0.0f;
}

float GroundAt(float x, float y, float fromZ, bool &found) {
	using GroundFn = float(__cdecl *)(float, float, float, bool *);
	found = false;
	return Func<GroundFn>(CWorld__FindGroundZFor3DCoord)(x, y, fromZ, &found);
}

// Nothing between the toucher and a place round him: no wall, no car, no
// object, by CWorld::GetIsLineOfSightClear, the test the mission's own ring
// uses (game/mission.cpp, Teleport), at a pedestrian's middle.
bool PlaceInSight(float x, float y, float dx, float dy, float z) {
	struct Vec3f {
		float x, y, z;
	};
	using LineOfSightFn = bool(__cdecl *)(const Vec3f *, const Vec3f *, int, int, int, int, int,
	                                      int, int);
	const Vec3f from{x, y, z}, to{x + dx, y + dy, z};
	return Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(&from, &to, 1, 1, 0, 1, 0, 0, 0) !=
	       0;
}

void Report(uint8_t result) {
	if (g_move.vote)
		g_client->ReportRampageArrival(g_move.body.voteId, result);
}

// Our copies of the cars other players sit in near (x, y), for the move to
// keep clear of (MoveKeepOut), and whose each one is. Our own player's car is
// not one: he is taken out of it first.
size_t KeepOutsNear(float x, float y, MoveKeepOut *out, uint8_t *whose, size_t max) {
	void  *seen[MOVE_KEEP_OUTS] = {};
	size_t n                    = 0;
	for (uint8_t id = 0; id < MAX_PLAYERS && n < max && n < MOVE_KEEP_OUTS; ++id) {
		void *const car = CarPlayerSitsIn(id);
		if (!car)
			continue;
		bool twice = false;   // two players in one car
		for (size_t i = 0; i < n; ++i)
			twice = twice || seen[i] == car;
		if (twice)
			continue;
		const float *p  = &Field<float>(car, offs::POSITION);
		const float  dx = p[0] - x, dy = p[1] - y;
		if (dx * dx + dy * dy > MOVE_KEEP_OUT_NEAR_M * MOVE_KEEP_OUT_NEAR_M)
			continue;
		const float *v = &Field<float>(car, offs::MOVE_SPEED);
		MoveKeepOut  k;
		k.x      = p[0];
		k.y      = p[1];
		k.vx     = v[0] * ENGINE_STEPS_PER_SECOND;
		k.vy     = v[1] * ENGINE_STEPS_PER_SECOND;
		k.radius = Func<float(__thiscall *)(void *)>(CEntity__GetBoundRadius)(car);
		if (!(k.radius > 0.0f && k.radius < MOVE_CAR_RADIUS_MAX_M))
			k.radius = MOVE_CAR_RADIUS_FALLBACK_M;
		seen[n]  = car;
		whose[n] = id;
		out[n++] = k;
	}
	return n;
}

bool g_saidKeptOff = false;

// On the ground, in his place round the toucher, facing him. Never on a car
// another player sits in (PickMoveSpot).
void Settle(void *ped) {
	const Vec3 &at = g_move.body.pos;

	bool        starterFound = false;
	const float starterGround = GroundAt(at.x, at.y, at.z + 1.5f, starterFound);
	const float groundRef     = starterFound ? starterGround : at.z - 1.0f;

	MoveKeepOut  cars[MOVE_KEEP_OUTS];
	uint8_t      whose[MOVE_KEEP_OUTS];
	const size_t n = KeepOutsNear(at.x, at.y, cars, whose, MOVE_KEEP_OUTS);

	float lastGround = groundRef;
	const MoveSpotPick pick =
	    PickMoveSpot(at.x, at.y, g_move.body.slot, g_move.body.count, cars, n,
	                 [&](float sx, float sy) {
		                 bool        found = false;
		                 const float g     = GroundAt(sx, sy, groundRef + 2.0f, found);
		                 if (!VoteSpreadGroundOk(found, g, groundRef) ||
		                     !PlaceInSight(at.x, at.y, sx - at.x, sy - at.y, groundRef + 1.0f))
			                 return false;
		                 lastGround = g;
		                 return true;
	                 });

	float x = at.x, y = at.y, ground = groundRef;
	const int used = pick.attempt;
	if (used >= 0) {
		x = at.x + pick.dx;
		y = at.y + pick.dy;
		if (pick.groundOk) {
			ground = lastGround;
		} else {
			bool        found = false;
			const float g     = GroundAt(x, y, groundRef + 2.0f, found);
			ground            = found ? g : groundRef;
		}
	}

	// The line that says a car was there, once: whose, how far off it he went,
	// and whether the ring had to go wide for it.
	if (n != 0 && !g_saidKeptOff) {
		g_saidKeptOff = true;
		size_t closest = 0;
		for (size_t i = 1; i < n; ++i)
			if (SpotClearance(x, y, cars[i]) < SpotClearance(x, y, cars[closest]))
				closest = i;
		const char *to    = g_client->NickFor(g_move.body.starterId);
		const char *owner = g_client->NickFor(whose[closest]);
		Log("%s: going beside %s, with %u car(s) other players sit in closest the spot, %s's the "
		    "nearest (%.1f m/s); put down on try %d of the car ring, %.1f m outside it and the "
		    "road it covers%s",
		    Tag(), to ? to : "?", static_cast<unsigned>(n), owner ? owner : "?",
		    std::sqrt(cars[closest].vx * cars[closest].vx + cars[closest].vy * cars[closest].vy), used,
		    SpotClearance(x, y, cars[closest]),
		    pick.covered ? " - every place round the spot was covered, this is the least bad" : "");
	}

	using BaseFn   = float(__thiscall *)(void *);
	const float up = Func<BaseFn>(CEntity__GetDistanceFromCentreOfMassToBaseOfModel)(ped);
	const float z  = ground + up;

	// The scene round the spot first, the way LOAD_SCENE does it.
	const float scene[3] = {x, y, z};
	Func<void(__cdecl *)()>(CTimer__Stop)();
	Func<void(__cdecl *)(const float *)>(CStreaming__LoadScene)(scene);
	Func<void(__cdecl *)()>(CTimer__Update)();

	TeleportPed(ped, x, y, z);
	const float heading = HeadingToward(x, y, at.x, at.y);
	Field<float>(ped, offs::PED_ROT_CUR)  = WrapAngle(heading);
	Field<float>(ped, offs::PED_ROT_DEST) = WrapAngle(heading);

	// Not CTheScripts::ClearSpaceForMissionEntity, which the warp handler
	// calls next: it takes cars out of the way by removing them, and a car
	// parked next to the toucher is as likely as not somebody's session car.
	// A ped landing against one gets pushed off it by the physics.

	const char *nick = g_client->NickFor(g_move.body.starterId);
	if (used < 0)
		Log("%s: no ground in the ring round %s, put down right beside him at "
		    "(%.1f %.1f %.1f)",
		    Tag(), nick ? nick : "?", x, y, z);
	else
		Log("%s: moved next to %s, place %u of %u (try %d), at (%.1f %.1f %.1f)%s",
		    Tag(), nick ? nick : "?", g_move.body.slot + 1, g_move.body.count, used, x, y, z,
		    g_move.locked ? " - an island our story hasn't opened yet" : "");
	Report(g_move.locked ? RAMPAGE_ARRIVED_LOCKED : RAMPAGE_ARRIVED);
	g_move.active = false;
}

void StartMove(const RampageTeleportBody &body, bool vote = true,
               const char *tag = "missions") {
	g_move        = Move{};
	g_move.vote   = vote;
	g_move.tag    = tag;
	g_move.body   = body;

	void *const   ped   = PlayerPed();
	TeleportFacts facts = ReadFacts(ped);
	// The session's mission is why a participant is moved, not a reason not to.
	if (!vote)
		facts.onMission = false;
	const uint8_t verdict = DecideTeleport(facts);
	if (verdict != RAMPAGE_ARRIVED) {
		const char *nick = g_client->NickFor(body.starterId);
		Log("%s: not moving to %s: %s.%s", Tag(), nick ? nick : "?", WhyNot(verdict),
		    vote ? " The vote still counted" : "");
		Report(verdict);
		return;
	}

	OutOfAnyCar(ped);

	g_move.level      = IslandAt(body.pos.x, body.pos.y, body.pos.z);
	g_move.locked     =!IslandOpen(g_move.level, Global<int32_t>(CStats__IndustrialPassed) != 0,
	                                Global<int32_t>(CStats__CommercialPassed) != 0);
	if (g_move.locked)
		Log("%s: going to island %d, which our story hasn't opened yet. Going "
		    "anyway, to be with the others",
		    Tag(), g_move.level);

	const int32_t loaded = IslandLoaded();
	g_move.active        = true;
	if (IslandToLoadFirst(g_move.level, loaded) == 0) {
		Settle(ped);
		return;
	}

	// Another island. Put him where the toucher stands, at the toucher's
	// height: this frame's CCollision::Update sees him there and loads that
	// island before CWorld::Process runs, and next frame he is put down
	// properly.
	// Beside a car another player sits in, not in it: the first place of the
	// car ring, which is on the same island.
	MoveKeepOut          cars[MOVE_KEEP_OUTS];
	uint8_t              whose[MOVE_KEEP_OUTS];
	const size_t         n    = KeepOutsNear(body.pos.x, body.pos.y, cars, whose, MOVE_KEEP_OUTS);
	const VoteSpreadSpot hold = n != 0 ? MoveSpotTry(body.slot, body.count, 0, n) : VoteSpreadSpot{};
	g_move.hold[0] = body.pos.x + hold.dx;
	g_move.hold[1] = body.pos.y + hold.dy;
	g_move.hold[2] = body.pos.z + 0.5f;
	TeleportPed(ped, g_move.hold[0], g_move.hold[1], g_move.hold[2]);
	Log("%s: the one we go to is on island %d and we have %d loaded; waiting for the "
	    "engine to load it",
	    Tag(), g_move.level, loaded);
}

void TickMove() {
	if (!g_move.active)
		return;
	void *const ped = PlayerPed();
	if (!ped) {
		g_move.active = false;
		Report(RAMPAGE_SKIPPED_NO_PED);
		return;
	}
	const int32_t loaded = Global<int32_t>(CCollision__ms_collisionInMemory);
	if (loaded == g_move.level) {
		Log("%s: island %d is loaded after %u frame(s)", Tag(), g_move.level, g_move.frames);
		Settle(ped);
		return;
	}
	if (++g_move.frames >= COLLISION_WAIT_FRAMES) {
		Log("%s: island %d never loaded (still %d after %u frames); putting him "
		    "down anyway",
		    Tag(), g_move.level, loaded, g_move.frames);
		Settle(ped);
		return;
	}
	// Held where he was put until the ground is there to stand on.
	TeleportPed(ped, g_move.hold[0], g_move.hold[1], g_move.hold[2]);
}

} // namespace

void SetRampageVoteKeys(int yesKey, int noKey) {
	if (yesKey > 0 && yesKey < 256)
		g_yesKey = yesKey;
	if (noKey > 0 && noKey < 256)
		g_noKey = noKey;
}

void InstallRampageVote(Client &client) {
	g_client = &client;
	char y[8], n[8];
	RampageKeyName(y, sizeof y, g_yesKey);
	RampageKeyName(n, sizeof n, g_noKey);
	Log("rampage: a skull is put to a vote in a shared session; %s says yes, %s says no", y, n);
}

void RemoveRampageVote() { g_client = nullptr; }

int32_t IslandAt(float x, float y, float z) {
	const float at[3] = {x, y, z};
	using LevelFn     = uint8_t(__cdecl *)(const float *);
	return Func<LevelFn>(CTheZones__GetLevelFromPosition)(at);
}

int32_t IslandLoaded() { return Global<int32_t>(CCollision__ms_collisionInMemory); }

bool MayMovePlayer() {
	if (!g_client || g_move.active)
		return false;
	TeleportFacts facts = ReadFacts(PlayerPed());
	facts.onMission     = false;
	return DecideTeleport(facts) == RAMPAGE_ARRIVED;
}

void *CarPlayerSitsIn(uint8_t playerId) {
	if (!g_client || playerId >= MAX_PLAYERS || playerId == g_client->LocalPlayerId())
		return nullptr;
	const RemotePlayer &p = g_client->PlayerSlot(playerId);
	if (!p.active)
		return nullptr;
	using GetFn = void *(__cdecl *)(int32_t);
	if (p.poolHandle >= 0)
		if (void *const ped = Func<GetFn>(CPools__GetPed)(p.poolHandle))
			if (Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0)
				if (void *const car = Field<void *>(ped, offs::PED_MY_VEHICLE))
					return car;
	const uint16_t netId = p.Seated() ? p.seatedVehicleNetId : p.seatVehicleNetId;
	if (netId == INVALID_NETID)
		return nullptr;
	const int32_t handle = g_client->SessionCarHandleOf(netId);
	return handle >= 0 ? Func<GetFn>(CPools__GetVehicle)(handle) : nullptr;
}

float CarSpeedMps(void *car) {
	if (!car)
		return 0.0f;
	const float *v = &Field<float>(car, offs::MOVE_SPEED);
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * ENGINE_STEPS_PER_SECOND;
}

bool MovePlayerBeside(const Vec3 &pos, uint8_t targetId, uint8_t slot, uint8_t count,
                      const char *tag) {
	if (!MayMovePlayer())
		return false;
	RampageTeleportBody body{};
	body.starterId = targetId;
	body.slot      = slot;
	body.count     = count;
	body.pos       = pos;
	StartMove(body, false, tag ? tag : "missions");
	return true;
}

TeleportFacts ReadTeleportFacts() { return ReadFacts(PlayerPed()); }

bool MoveInProgress() { return g_move.active; }

void TickRampageVote() {
	if (!g_client)
		return;
	const RampageVoteView &v   = g_client->VoteView();
	const uint32_t         now = WallClock::NowMs();

	RampageTeleportBody body{};
	const bool took = g_client->TakeRampageTeleport(body);
	if (took && !InsideWorld(body.pos.x, body.pos.y, body.pos.z)) {
		// The starter's last reported place, which nothing checked on its way
		// here; our player would be filed in the sector grid at it.
		Log("rampage: the move's place (%.1f, %.1f, %.1f) is off the map; staying put",
		    body.pos.x, body.pos.y, body.pos.z);
		TickMove();
	} else if (took) {
		if (g_move.active)
			Log("rampage: a second move arrived before the first finished; taking the new one");
		StartMove(body);
	} else {
		TickMove();
	}

	// Nothing to draw or read with no player in the world - the title
	// screen, a loading screen.
	if (PlayerPed() == nullptr)
		return;
	TickHelpBox(v, now);
	TickKeys(v);
}

} // namespace coopiii::game
