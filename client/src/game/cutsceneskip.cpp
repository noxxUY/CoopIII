#include "cutsceneskip.h"

#include "addresses.h"
#include "chat.h"
#include "leadcheck.h"
#include "../client.h"
#include "../clock.h"
#include "../cutsceneskipview.h"
#include "../log.h"

#include <coopiii/mission.h>

#include <windows.h>

#include <cstring>

namespace coopiii::game {

namespace {

using VoidFn    = void(__cdecl *)();
using CompareFn = void(__thiscall *)(void *, bool);

Client *g_client      = nullptr;
bool    g_finishCall  = false;   // 0x00405131 comes to OnSkipInput
bool    g_compareCall = false;   // 0x0043DBED comes to OnButtonCompare

// The scene this game is in, as last told to the server.
bool        g_haveKey     = false;
CutsceneKey g_key{};
bool        g_skippedHere = false;   // taken to its end here already
bool        g_saidNothing = false;   // a press that was not ours to act on, logged once

// A skip the server ordered before this game could carry it out: a
// participant's replay a frame or two behind its owner's.
bool        g_pending = false;
CutsceneKey g_pendingKey{};
uint8_t     g_pendingVote    = 0;
uint32_t    g_pendingUntilMs = 0;
constexpr uint32_t PENDING_MS = 3000;

// How many frames the intro's own skip question is answered "pressed" after
// the count said skip. It is asked every frame of the intro and jumps on the
// first yes; the rest is margin.
int           g_introForce        = 0;
constexpr int INTRO_FORCE_FRAMES  = 10;

// Points the `call` at `site` at `to`, only while it still calls `from`.
bool RedirectCall(uintptr_t site, uintptr_t from, uintptr_t to) {
	if (!RelCallAt(Ptr<uint8_t>(site), site, from))
		return false;
	DWORD old = 0;
	if (!VirtualProtect(reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &old))
		return false;
	const int32_t rel = static_cast<int32_t>(to - (site + 5));
	std::memcpy(reinterpret_cast<void *>(site + 1), &rel, sizeof rel);
	VirtualProtect(reinterpret_cast<void *>(site), 5, old, &old);
	FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(site), 5);
	return true;
}

bool Started() { return Global<uint8_t>(CCutsceneMgr__ms_started) != 0; }
bool PlayingIntro() { return Global<uint8_t>(CGame__playingIntro) != 0; }

void EngineName(char (&out)[CUTSCENE_NAME_LEN + 1]) {
	std::memcpy(out, Ptr<const char>(CCutsceneMgr__ms_cutsceneName), CUTSCENE_NAME_LEN);
	out[CUTSCENE_NAME_LEN] = '\0';
}

// Update's test before it reads a single input: running, not the credits,
// the active camera flying the scene's spline, the load settled.
bool Skippable() {
	if (!Started())
		return false;
	char name[CUTSCENE_NAME_LEN + 1];
	EngineName(name);
	if (IsCreditsCutscene(name))
		return false;
	const uint8_t cam  = Global<uint8_t>(TheCamera__ActiveCam);
	const int16_t mode = Global<int16_t>(TheCamera__Cams_Mode + cam * CAMERA_CAM_STRIDE);
	return mode == CAM_MODE_FLYBY && Global<uint32_t>(CCutsceneMgr__ms_cutsceneLoadStatus) == 0;
}

bool Connected() {
	return g_client && g_client->IsConnected() && g_client->LocalPlayerId() < MAX_PLAYERS;
}

struct Whose {
	uint8_t scope    = CUTSCENE_SCOPE_OWN;
	bool    follower = false;
};

Whose WhoseScene() {
	Whose w;
	if (!Connected())
		return w;
	const MissionSync &m      = g_client->Missions();
	const uint8_t      local  = g_client->LocalPlayerId();
	const bool         owner  = m.Running() && m.Owner() == local;
	const bool         helper = m.Running() && (m.Participants() & PlayerBit(local)) != 0;
	w.scope    = CutsceneScopeFor(m.Shared() && m.Running(), m.Busy(), owner, helper);
	w.follower = w.scope == CUTSCENE_SCOPE_SHARED && !owner;
	return w;
}

CutsceneKey CurrentKey() {
	char name[CUTSCENE_NAME_LEN + 1];
	EngineName(name);
	return MakeCutsceneKey(WhoseScene().scope, name);
}

// FinishCutscene, the way Update calls it.
void FinishHere(const char *why) {
	Func<VoidFn>(CCutsceneMgr__FinishCutscene)();
	g_skippedHere = true;
	const bool intro = PlayingIntro();
	if (intro)
		g_introForce = INTRO_FORCE_FRAMES;
	char name[CUTSCENE_NAME_LEN + 1];
	EngineName(name);
	Log("cutscene: skipped '%s' here, %s%s", name, why, intro ? "; the intro skips itself too" : "");
}

bool OursInTheView(const CutsceneKey &key) {
	const CutsceneSkipView &v = g_client->SkipView();
	return v.In() && SameCutscene(v.reported, key);
}

// CCutsceneMgr::Update's call to FinishCutscene. The game has said yes to a
// skip by the time it gets here.
void __cdecl OnSkipInput() {
	if (!g_client) {
		Func<VoidFn>(CCutsceneMgr__FinishCutscene)();
		return;
	}
	const Whose             w    = WhoseScene();
	const CutsceneKey       key  = CurrentKey();
	const CutsceneSkipView &v    = g_client->SkipView();
	const bool              ours = OursInTheView(key);
	const SkipPress press = DecideSkipPress(Connected(), w.scope, w.follower, ours && v.Crowded(),
	                                        ours && v.HaveCast());
	char name[CUTSCENE_NAME_LEN + 1];
	CutsceneKeyName(key, name);
	switch (press) {
	case SkipPress::Local:
		FinishHere(Connected() ? "nobody else is in it" : "no session");
		break;
	case SkipPress::Vote:
		if (g_client->CastCutsceneSkip())
			Log("cutscene: skip pressed in '%s', a vote now (%u/%u, %u needed)", name, v.vote.yes + 1u,
			    v.vote.voters, v.vote.needed);
		break;
	case SkipPress::Nothing:
		if (!g_saidNothing) {
			g_saidNothing = true;
			Log("cutscene: skip pressed in '%s' and not acted on: %s", name,
			    v.HaveCast() ? "we said skip already" : "the mission's owner skips its scene");
		}
		break;
	}
}

// Held for the intro while somebody else is in it with us and we haven't
// skipped: its own question about Cross and Start is answered "no".
bool HoldIntroSkip() {
	if (!Connected() || !g_haveKey || g_skippedHere)
		return false;
	const CutsceneSkipView &v = g_client->SkipView();
	return OursInTheView(g_key) && v.Crowded();
}

// IS_BUTTON_PRESSED handing its answer to CRunningScript::UpdateCompareFlag.
// __thiscall with one argument and `ret 4`; __fastcall with the spare edx is
// the same call.
void __fastcall OnButtonCompare(void *script, void * /*edx*/, int value) {
	bool pressed = (value & 0xFF) != 0;
	if (PlayingIntro()) {
		const int32_t *params = Ptr<int32_t>(CTheScripts__ScriptParams);
		if (params[0] == 0 && (params[1] == PAD_BUTTON_START || params[1] == PAD_BUTTON_CROSS)) {
			if (g_introForce > 0)
				pressed = true;
			else if (pressed && HoldIntroSkip())
				pressed = false;
		}
	}
	Func<CompareFn>(CRunningScript__UpdateCompareFlag)(script, pressed);
}

} // namespace

bool InstallCutsceneSkip(Client &client) {
	g_client = &client;
	if (!g_finishCall)
		g_finishCall = RedirectCall(CCutsceneMgr__Update_FinishCall, CCutsceneMgr__FinishCutscene,
		                            reinterpret_cast<uintptr_t>(&OnSkipInput));
	if (g_finishCall)
		Log("cutscene: CCutsceneMgr::Update's call to FinishCutscene at 0x%08X comes to us; with "
		    "somebody else in the scene the skip input is a vote",
		    static_cast<unsigned>(CCutsceneMgr__Update_FinishCall));
	else
		Log("cutscene: FAILED to take the call to FinishCutscene at 0x%08X; every skip stays "
		    "this machine's own",
		    static_cast<unsigned>(CCutsceneMgr__Update_FinishCall));

	if (!g_compareCall)
		g_compareCall = RedirectCall(IS_BUTTON_PRESSED_CompareCall, CRunningScript__UpdateCompareFlag,
		                             reinterpret_cast<uintptr_t>(&OnButtonCompare));
	if (g_compareCall)
		Log("cutscene: IS_BUTTON_PRESSED's answer at 0x%08X comes to us, for the intro's own skip",
		    static_cast<unsigned>(IS_BUTTON_PRESSED_CompareCall));
	else
		Log("cutscene: FAILED to take IS_BUTTON_PRESSED's answer at 0x%08X; the intro can still "
		    "be skipped alone by its own button test",
		    static_cast<unsigned>(IS_BUTTON_PRESSED_CompareCall));
	return g_finishCall;
}

void RemoveCutsceneSkip() {
	if (g_compareCall)
		RedirectCall(IS_BUTTON_PRESSED_CompareCall, reinterpret_cast<uintptr_t>(&OnButtonCompare),
		             CRunningScript__UpdateCompareFlag);
	if (g_finishCall)
		RedirectCall(CCutsceneMgr__Update_FinishCall, reinterpret_cast<uintptr_t>(&OnSkipInput),
		             CCutsceneMgr__FinishCutscene);
	g_compareCall = false;
	g_finishCall  = false;
	g_client      = nullptr;
}

bool CutsceneSkipRedirected() { return g_finishCall; }

void TickCutsceneSkip() {
	if (!g_client)
		return;
	if (g_introForce > 0 && (--g_introForce == 0 || !PlayingIntro()))
		g_introForce = 0;

	// What the game is in. Kept while the scene runs even if the test drops
	// for a frame; gone once the scene is.
	if (Skippable()) {
		const CutsceneKey key = CurrentKey();
		if (!g_haveKey || !SameCutscene(key, g_key)) {
			g_key         = key;
			g_haveKey     = true;
			g_skippedHere = false;
			g_saidNothing = false;
		}
	} else if (!Started() && g_haveKey) {
		g_haveKey     = false;
		g_skippedHere = false;
		g_saidNothing = false;
	}

	if (!Connected()) {
		g_pending = false;
		return;
	}
	g_client->ReportCutscene(g_haveKey ? g_key : CutsceneKey{});

	const uint32_t now = WallClock::NowMs();
	CutsceneKey    key{};
	uint8_t        voteId = 0;
	if (g_client->TakeCutsceneSkip(key, voteId)) {
		g_pending        = true;
		g_pendingKey     = key;
		g_pendingVote    = voteId;
		g_pendingUntilMs = now + PENDING_MS;
	}
	if (!g_pending)
		return;
	char name[CUTSCENE_NAME_LEN + 1];
	CutsceneKeyName(g_pendingKey, name);
	if (g_haveKey && SameCutscene(g_key, g_pendingKey) && Skippable()) {
		g_pending = false;
		if (g_skippedHere)
			Log("cutscene: '%s' was at its end here already when the vote's skip came", name);
		else
			FinishHere(g_key.scope == CUTSCENE_SCOPE_SHARED && WhoseScene().follower
			               ? "with everybody in it; the owner's skip moves the mission on"
			               : "with everybody in it");
	} else if (static_cast<int32_t>(now - g_pendingUntilMs) >= 0) {
		g_pending = false;
		Log("cutscene: the skip of '%s' (vote %u) found no such scene to skip here", name,
		    g_pendingVote);
	}
}

void DrawCutsceneSkip() {
	if (!g_client || !g_haveKey || g_skippedHere || !Connected())
		return;
	const CutsceneSkipView &v = g_client->SkipView();
	if (!v.Crowded() || !SameCutscene(v.reported, g_key))
		return;
	char text[48];
	FormatSkipCounter(text, sizeof text, v.vote.yes, v.vote.voters, v.HaveCast());
	DrawCornerMark(text);
}

} // namespace coopiii::game
