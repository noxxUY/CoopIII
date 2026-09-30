#include "standapart.h"

#include "addresses.h"
#include "ped.h"
#include "rampagevote.h"
#include "../client.h"
#include "../clock.h"
#include "../log.h"

#include <cstring>

namespace coopiii::game {

namespace {

Client *g_client = nullptr;

StandWatch g_watch;
uint32_t   g_lastMs = 0;

// What last put our player down, and whether it has been said yet for each
// kind of move (a line per placement and kind, not one a frame).
const char *g_placedBy  = "standing where the game put us";
bool        g_saidSlot  = false;
bool        g_saidNudge = false;
bool        g_saidNone  = false;
bool        g_freshNote = false;   // said since the last frame

// For telling a jump from a walk, and a game out of its own mission.
bool  g_haveLast = false;
float g_last[3]  = {};
bool  g_wasBusy  = false;

// A spot with no clear place round it is not looked round again every frame.
constexpr uint32_t SLOT_RETRY_MS  = 1000;
uint32_t           g_slotFailedMs = 0;
bool               g_slotFailed   = false;

struct Vec3f {
	float x, y, z;
};
// CWorld::GetIsLineOfSightClear, the test the mission's ring uses
// (game/mission.cpp, Teleport): buildings, vehicles, peds, objects, dummies,
// see-through, some objects.
using LineOfSightFn = bool(__cdecl *)(const Vec3f *, const Vec3f *, int, int, int, int, int, int,
                                      int);

void *PlayerPed() { return Func<void *(__cdecl *)()>(FindPlayerPed)(); }

bool Clear(float fx, float fy, float fz, float tx, float ty, float tz) {
	const Vec3f from{fx, fy, fz}, to{tx, ty, tz};
	return Func<LineOfSightFn>(CWorld__GetIsLineOfSightClear)(&from, &to, 1, 1, 0, 1, 0, 0, 0) !=
	       0;
}

float GroundAt(float x, float y, float fromZ, bool &found) {
	using GroundFn = float(__cdecl *)(float, float, float, bool *);
	found = false;
	return Func<GroundFn>(CWorld__FindGroundZFor3DCoord)(x, y, fromZ, &found);
}

// On foot, and not on the way into or out of a car.
bool OnFoot(void *ped) {
	if (Field<uint8_t>(ped, offs::PED_IN_VEHICLE) != 0)
		return false;
	const uint32_t s = Field<uint32_t>(ped, offs::PED_STATE);
	return s != PEDSTATE_ENTER_CAR && s != PEDSTATE_CARJACK && s != PEDSTATE_DRAG_FROM_CAR &&
	       s != PEDSTATE_EXIT_CAR;
}

// Our place round the other player's spot, on the first ring whose place the
// buildings, cars and objects leave clear and whose ground is the other
// player's. The engine's own CPed::Teleport through the vtable, which is what
// SET_PLAYER_COORDINATES does to a player on foot, and which leaves the
// heading alone.
bool TakeSlot(void *ped, const StandVerdict &v, uint8_t *usedRing, uint8_t *usedAttempt,
              float *dist) {
	const float *p       = &Field<float>(ped, offs::POSITION);
	const float  midZ    = p[2];
	bool         found   = false;
	const float  groundA = GroundAt(v.anchor.x, v.anchor.y, midZ + 1.5f, found);
	const float  ref     = found ? groundA : midZ - 1.0f;
	for (uint8_t ring = 0; ring < 2; ++ring) {
		for (uint8_t attempt = 0; attempt < SPREAD_ATTEMPTS; ++attempt) {
			if (SpreadSpotTaken(v.rank, v.count, attempt))
				continue;
			float cx = 0.0f, cy = 0.0f;
			SpreadSpot(v.anchor.x, v.anchor.y, v.rank, v.count, attempt, &cx, &cy,
			           STAND_SLOT_RADII_M[ring]);
			bool        under = false;
			const float g     = GroundAt(cx, cy, ref + 2.0f, under);
			if (!under || std::fabs(g - ref) > STAND_MAX_STEP_M)
				continue;
			if (!Clear(v.anchor.x, v.anchor.y, midZ, cx, cy, midZ))
				continue;
			using BaseFn   = float(__thiscall *)(void *);
			const float up = Func<BaseFn>(CEntity__GetDistanceFromCentreOfMassToBaseOfModel)(ped);
			const float heading = Field<float>(ped, offs::PED_ROT_CUR);
			using TeleportFn    = void(__thiscall *)(void *, float, float, float);
			void *const *vtable = *reinterpret_cast<void *const *const *>(ped);
			reinterpret_cast<TeleportFn>(vtable[11])(ped, cx, cy, g + up);
			float *const vel = &Field<float>(ped, offs::MOVE_SPEED);
			vel[0] = vel[1] = vel[2] = 0.0f;
			Field<float>(ped, offs::PED_ROT_CUR)  = heading;
			Field<float>(ped, offs::PED_ROT_DEST) = heading;
			*usedRing    = ring;
			*usedAttempt = attempt;
			*dist        = STAND_SLOT_RADII_M[ring];
			return true;
		}
	}
	return false;
}

// A step away, where the line ahead is clear. Put down the way a replica is,
// matrix, RenderWare frame and sector lists, at the heading it has, so the
// frame's own collision sees it where it now is.
bool StepAway(void *ped, const StandVerdict &v, uint32_t dtMs) {
	const float step = StandNudgeStep(v.apartM, dtMs);
	if (step <= 0.0f)
		return true;
	const float *p  = &Field<float>(ped, offs::POSITION);
	const float  tx = p[0] + v.awayX * step, ty = p[1] + v.awayY * step;
	// Tested a pedestrian's width further on, so the step does not end
	// against a wall.
	if (!Clear(p[0], p[1], p[2], tx + v.awayX * 0.35f, ty + v.awayY * 0.35f, p[2]))
		return false;
	PlaceReplicaPed(ped, Vec3{tx, ty, p[2]}, Field<float>(ped, offs::PED_ROT_CUR), true);
	return true;
}

} // namespace

void InstallStandApart(Client &client) {
	g_client = &client;
	g_watch.Reset();
	Log("standapart: a player put on another's spot moves to a place of its own beside it");
}

void RemoveStandApart() { g_client = nullptr; }

void NoteStandPlacement(const char *what) {
	if (!what)
		return;
	g_placedBy  = what;
	g_saidSlot  = false;
	g_saidNudge = false;
	g_saidNone  = false;
	g_freshNote = true;
}

void TickStandApart() {
	if (!g_client || !g_client->IsConnected())
		return;
	const uint32_t now = WallClock::NowMs();
	const uint32_t dt  = g_lastMs == 0 ? 0 : now - g_lastMs;
	g_lastMs           = now;
	const uint8_t localId = g_client->LocalPlayerId();
	void *const   ped     = PlayerPed();
	if (localId >= MAX_PLAYERS || !ped) {
		g_watch.Reset();
		g_haveLast = false;
		return;
	}
	const float *p = &Field<float>(ped, offs::POSITION);

	// What put us here, for the line below: the end of this game's own
	// intro or info scene, or a jump no walk makes.
	const MissionSync &m    = g_client->Missions();
	const bool         busy = m.Busy();
	if (g_wasBusy && !busy)
		NoteStandPlacement("the end of this game's own intro or info scene");
	g_wasBusy = busy;
	if (g_haveLast) {
		const float dx = p[0] - g_last[0], dy = p[1] - g_last[1];
		if (!g_freshNote && dx * dx + dy * dy > 20.0f * 20.0f)
			NoteStandPlacement("a jump (a respawn, a load or a scripted move)");
	}
	g_last[0] = p[0];
	g_last[1] = p[1];
	g_last[2] = p[2];
	g_haveLast  = true;
	g_freshNote = false;

	// Only a player who can be moved at all: on foot, alive, not arrested,
	// not in a cutscene, not on the way somewhere, and not in the middle of
	// its own intro, which moves it round the marker itself.
	if (busy || !OnFoot(ped) || !MayMovePlayer()) {
		g_watch.Reset();
		return;
	}

	StandPeer peers[MAX_PLAYERS];
	size_t    n       = 0;
	uint8_t   players = PlayerBit(localId);
	for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
		if (id == localId)
			continue;
		const RemotePlayer &r = g_client->PlayerSlot(id);
		if (!r.active)
			continue;
		players = static_cast<uint8_t>(players | PlayerBit(id));
		if (!r.haveState || r.dead || r.Seated() || r.Entering())
			continue;
		peers[n].playerId = id;
		peers[n].pos      = r.last.pos;
		++n;
	}
	const uint8_t      first = m.Running() ? m.Owner() : INVALID_PLAYER;
	const Vec3         me{p[0], p[1], p[2]};
	const StandVerdict v = DecideStand(localId, me, peers, n, players, first);
	if (!g_watch.Due(v, now))
		return;

	const char *nick = g_client->NickFor(v.anchorId);
	if (g_slotFailed && static_cast<uint32_t>(now - g_slotFailedMs) >= SLOT_RETRY_MS)
		g_slotFailed = false;
	if (v.move == StandMove::Slot && !g_slotFailed) {
		uint8_t ring = 0, attempt = 0;
		float   dist = 0.0f;
		if (TakeSlot(ped, v, &ring, &attempt, &dist)) {
			g_watch.Reset();
			if (!g_saidSlot) {
				g_saidSlot = true;
				Log("standapart: after %s we stood on %s's spot (%.1f, %.1f); moved to place %u "
				    "of %u round it, %.1f m out (ring %u, try %u), same heading",
				    g_placedBy, nick ? nick : "?", v.anchor.x, v.anchor.y, v.rank + 1u,
				    static_cast<unsigned>(v.count), dist, static_cast<unsigned>(ring),
				    static_cast<unsigned>(attempt));
			}
			return;
		}
		g_slotFailed   = true;
		g_slotFailedMs = now;
		if (!g_saidNone) {
			g_saidNone = true;
			Log("standapart: after %s we stood on %s's spot (%.1f, %.1f) and no place round it "
			    "is clear; stepping away instead",
			    g_placedBy, nick ? nick : "?", v.anchor.x, v.anchor.y);
		}
	}
	if (StepAway(ped, v, dt) && !g_saidNudge) {
		g_saidNudge = true;
		Log("standapart: after %s we stood %.2f m from %s; stepping away at %.1f m/s, place %u "
		    "of %u",
		    g_placedBy, v.apartM, nick ? nick : "?", STAND_NUDGE_MPS, v.rank + 1u,
		    static_cast<unsigned>(v.count));
	}
}

} // namespace coopiii::game
