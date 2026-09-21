// AgentPad.asi entry point.
//
// Standalone ASI, separate from CoopIII.asi. Its whole job is letting an
// external process drive GTA III's input, and it shares nothing with the
// co-op client at runtime. Reuses CoopIII's image guard and detour helper
// because those happen to solve the same two problems (wrong build, silent
// failure) - not because the mods are coupled in any way.
//
// Why this needs to exist: synthetic input just doesn't reach GTA III. Tried
// keybd_event, PostMessage with WM_KEYDOWN/WM_KEYUP, SendInput with
// KEYEVENTF_SCANCODE, synthetic mouse moves - all ignored by the live game,
// because it reads DirectInput device state directly and none of that touches
// it. Only way in is writing the game's own composed pad state from inside
// the process. That's what this does.
#include "cdstream.h"
#include "channel.h"
#include "inject.h"
#include "intro.h"
#include "settings.h"

#include "game/verify.h"
#include "game/addresses.h"
#include "hook/hook.h"
#include "log.h"

#include <string>

#include <windows.h>

using namespace coopiii;

namespace {

HMODULE                 g_module  = nullptr;
bool                    g_started = false;
agentpad::Settings      g_settings;
agentpad::CdStreamPatch g_cdstream;

// How long to wait for the game to reach GS_FRONTEND. Load-time budget, not a
// "wait for the player to pick a save" budget - fires on the title screen.
//
// Ten minutes, not two, because everything before GS_FRONTEND is basically
// unbounded in practice: legal screen, two FMVs the player may or may not
// skip, first streaming load, plus an ASI loader, Mod Loader, CLEO and six
// plugins on top. Measured 115s on the target install just to get a game
// going. A timeout that fires during a normal slow start looks exactly like a
// broken mod - docs/compat.md §2.5 is about avoiding that failure mode.
constexpr uint32_t READY_TIMEOUT_MS = 10 * 60 * 1000;

// Once the loop is turning, give every other plugin a beat to finish any
// patching it does on its first frames before we rewrite a prologue.
constexpr uint32_t SETTLE_MS = 1000;

std::string PathNextToModule(const char *leaf) {
	char buf[MAX_PATH] = {0};
	if (GetModuleFileNameA(g_module, buf, MAX_PATH) == 0)
		return leaf;
	std::string path(buf);
	const size_t slash = path.find_last_of("\\/");
	if (slash == std::string::npos)
		return leaf;
	return path.substr(0, slash + 1) + leaf;
}

// Readiness signal. Probably the one decision in this file that needs
// explaining.
//
// docs/compat.md §2.2 says don't hook from DllMain: SilentPatch, the
// Widescreen Fix, Framerate Vigilante and CLEO's III.MemoryModule all rewrite
// game code at runtime, and Mod Loader hasn't even run yet at DllMain time.
// So we need to wait for a signal that everyone else is done first.
// CoopIII.asi waits on CTimer::m_FrameCounter. This mod doesn't, on purpose,
// for two reasons.
//
// First: the frame counter is disputed. Static reading of the retail image
// says it should advance on the title screen - FrontendIdle (0x0048E700)
// opens with `call CTimer::Update` (0x004ACF70), CTimer::Update ends at
// 0x004AD2F1 with an unconditional `inc dword [0x009412EC]` both arms of the
// preceding branch fall through to, and WinMain's GS_FRONTEND case
// (0x00582E23) dispatches rsFRONTENDIDLE every frame the window isn't
// minimized. But measuring the live game with the real mod stack says
// otherwise: counter sat unchanged for 60s, then for 115s with the menu up,
// only moved once a save loaded. Neither measurement is explained yet. The
// leading guess is that something in the installed stack patched this path,
// which is the scenario docs/compat.md §2.2 is written for.
//
// Second, even settling that dispute wouldn't help - the frame counter is a
// proxy anyway. The real question is gGameState:
//
//   gGameState (0x008F5838) is what WinMain (0x00582710) switches on.
//   Reaching GS_FRONTEND (7) means WinMain's loop is turning, which means
//   WinMain got entered, which means every statically-linked DLL - the ASI
//   loader, so every .asi, so Mod Loader and its plugins too - already
//   finished DllMain. GS_FRONTEND's body is also exactly where
//   RsEventHandler(rsFRONTENDIDLE) -> FrontendIdle -> CPad::UpdatePads gets
//   called, and UpdatePads is the function we're about to hook.
//
// So waiting on gGameState answers the actual question directly, valid on the
// menu by construction, not inference. And it doesn't touch CTimer at all, so
// the dispute above doesn't matter either way.
//
// While waiting, we log both signals side by side, so the next in-game run
// settles the frame-counter question for free instead of costing another
// debugging session.
bool WaitForInputLoop(uint32_t timeoutMs) {
	auto       *state   = reinterpret_cast<volatile uint32_t *>(game::gGameState);
	const auto *counter = reinterpret_cast<const volatile uint32_t *>(game::CTimer__m_FrameCounter);

	const uint32_t counterAtStart = *counter;
	const DWORD    began          = GetTickCount();
	DWORD          lastReport     = began;
	bool           sawCounterMove = false;
	uint32_t       lastState      = 0xFFFFFFFFu;
	unsigned       skips          = 0;

	while (GetTickCount() - began < timeoutMs) {
		const uint32_t now     = *state;
		const uint32_t frames  = *counter;
		const DWORD    elapsed = GetTickCount() - began;

		if (frames != counterAtStart && !sawCounterMove) {
			sawCounterMove = true;
			Log("readiness: CTimer::m_FrameCounter first moved at %lu ms, with "
			    "gGameState = %u. (This is the disputed signal. If gGameState was 7 "
			    "here, the counter DOES run in the menu.)",
			    elapsed, now);
		}

		if (now != lastState) {
			lastState = now;
			Log("readiness: gGameState = %u%s (%lu ms, frame counter %s)", now,
			    now == game::GS_FRONTEND       ? " (GS_FRONTEND, the menu)"
			    : now == game::GS_PLAYING_GAME ? " (GS_PLAYING_GAME)"
			                                   : "",
			    elapsed, sawCounterMove ? "moving" : "still at its startup value");
		}

		// GS_FRONTEND is the earliest state where the function we hook runs
		// from a stable per-frame loop. Anything at or past it is fine - a fast
		// loader or an autoload can already be in GS_PLAYING_GAME the first
		// time we check.
		if (now >= game::GS_FRONTEND && now <= game::GS_PLAYING_GAME) {
			if (skips != 0)
				Log("intro: the frontend is up %lu ms after AgentPad started looking, with "
				    "the movies skipped (%u forced transition(s)).",
				    elapsed, skips);
			return true;
		}

		// Opt-in, off by default: shoves the game past its two startup movies.
		// Full argument for why 2->5 and 4->5 are the only safe jumps (and why
		// this must never fire from state 1, since the logo movie's
		// CoInitialize hasn't happened yet there) lives in intro.h. Read it
		// before touching ShouldSkipIntroFrom.
		//
		// The cap here is about noise, not correctness. WndProc can write 3
		// back underneath us when a movie ends on its own, so a couple extra
		// rounds is normal. Twenty would mean something's actively fighting us,
		// worth logging once instead of spamming forever.
		if (g_settings.skipIntro && skips < 8 && agentpad::ShouldSkipIntroFrom(now)) {
			*state = game::GS_INIT_ONCE;
			++skips;
			Log("intro: gGameState %u -> %u at %lu ms (skipping %s). The movie files are "
			    "untouched. This is the same transition the game makes when a player "
			    "presses a key, and the one-time init in states 5 and 6 still runs.",
			    now, static_cast<unsigned>(game::GS_INIT_ONCE), elapsed,
			    now == game::GS_LOGO_MPEG ? "both startup movies" : "the titles movie");
		}

		if (GetTickCount() - lastReport >= 15000) {
			lastReport = GetTickCount();
			Log("readiness: still waiting (%lu s), gGameState = %u. The intro "
			    "movies and the first load happen before GS_FRONTEND, so this "
			    "is normal for the first half-minute or two.",
			    elapsed / 1000, now);
		}
		Sleep(50);
	}
	return false;
}

DWORD WINAPI Boot(LPVOID) {
	LogOpen(PathNextToModule("AgentPad.log"));
	Log("AgentPad starting: external input driver for GTA III (pid %lu)",
	    GetCurrentProcessId());
	if (LogPath() != PathNextToModule("AgentPad.log"))
		Log("log: another instance already holds AgentPad.log, so this one is writing to "
		    "\"%s\" instead. Two processes appending to one log interleave into nonsense.",
		    LogPath().c_str());

	Log("settings: SkipIntro=%s, MultiInstance=%s", g_settings.skipIntro ? "on" : "off",
	    agentpad::Settings::MultiInstanceName(g_settings.multiInstance));
	Log("cdstream: %s%s%s", g_cdstream.reason, g_cdstream.applied ? " -> " : "",
	    g_cdstream.applied ? g_cdstream.name : "");

	const game::VerifyResult image = game::VerifyGameImage();
	Log("image: %s", image.detail.c_str());
	if (!image.ok) {
		Log("AgentPad will not load. No hooks were installed and the game is untouched.");
		return 0;
	}

	if (!WaitForInputLoop(READY_TIMEOUT_MS)) {
		Log("the game never reached GS_FRONTEND within %u s, giving up without "
		    "installing anything. gGameState is still %u.",
		    READY_TIMEOUT_MS / 1000,
		    *reinterpret_cast<const volatile uint32_t *>(game::gGameState));
		return 0;
	}
	Log("the frontend is up; WinMain's loop is turning and every plugin has loaded");
	Sleep(SETTLE_MS);

	if (!HookInit()) {
		for (const auto &f : HookFailures())
			Log("hook init: %s: %s", f.name.c_str(), f.reason.c_str());
		return 0;
	}

	if (!agentpad::InstallPadHook()) {
		HookShutdown();
		return 0;
	}
	g_started = true;

	// Injection stays off until a driver turns it on, so just installing this
	// mod never takes the keyboard away from a human. That's the default
	// behavior, not a setting - an input mod that grabs control on load would
	// be a genuinely bad guest.
	char section[agentpad::SHM_NAME_MAX] = {0};
	agentpad::FormatSectionName(GetCurrentProcessId(), section, sizeof(section));
	Log("AgentPad ready, pass-through until a driver enables injection. "
	    "Shared memory: \"%s\" (listed in \"%s\"). A driver with more than one "
	    "game running must say which: Get-GtaInstance, then -GamePid.",
	    section, agentpad::SHM_INDEX_NAME);
	return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		g_module = module;
		DisableThreadLibraryCalls(module);

		// Two things happen here and nowhere else - both are intentional
		// exceptions to the usual "DllMain starts a thread and returns" rule.
		//
		// Reading AgentPad.ini is one small CRT file read. It's here instead of
		// on the boot thread only because the second thing below needs the
		// answer and can't wait for it.
		//
		// The second thing is the streaming-semaphore rename, see cdstream.h.
		// Not a hook - no detour, no memory scan, no library load, so it
		// doesn't race the other mods over the same bytes the way
		// docs/compat.md §2.2 worries about. Has to happen here because the
		// game reads that string inside CGame::InitialiseOnceBeforeRW, which
		// runs before gGameState ever leaves GS_START_UP. No game-state signal
		// exists early enough to wait for instead, and spinning up a worker
		// thread would just race the game's own startup. DllMain is guaranteed
		// to run before that read. In the default "auto" mode this does
		// nothing at all unless another copy of the game is already running.
		g_settings.Load(PathNextToModule("AgentPad.ini"));
		g_cdstream = agentpad::ApplyCdStreamPatch(g_settings.multiInstance, GetCurrentProcessId());

		if (const HANDLE thread = CreateThread(nullptr, 0, &Boot, nullptr, 0, nullptr))
			CloseHandle(thread);
		break;

	case DLL_PROCESS_DETACH:
		if (g_started) {
			agentpad::RemovePadHook();
			HookShutdown();
		}
		LogClose();
		break;
	}
	return TRUE;
}
