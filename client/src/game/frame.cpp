#include "frame.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"

#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace coopiii::game {

namespace {

Detour                g_detour;
FrameFn               g_pre  = nullptr;
FrameFn               g_post = nullptr;
std::atomic<uint32_t> g_frames{0};

// CGame::Process is a static member (re3 Game.h), so __cdecl with no
// arguments. Get this wrong and the stack corrupts every single frame.
using ProcessFn = void(__cdecl *)();

void __cdecl HookedProcess() {
	// A throw escaping into the engine would unwind through frames that
	// know nothing about C++ exceptions. Contain it here instead - a
	// dropped frame of multiplayer beats taking the whole game down.
	if (g_pre) {
		try {
			g_pre();
		} catch (...) {
			Log("frame: pre-process callback threw; continuing");
		}
	}

	g_detour.Original<ProcessFn>()();

	if (g_post) {
		try {
			g_post();
		} catch (...) {
			Log("frame: post-process callback threw; continuing");
		}
	}

	g_frames.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

bool InstallFrameHook(FrameFn preProcess, FrameFn postProcess) {
	g_pre  = preProcess;
	g_post = postProcess;

	if (!g_detour.Install("CGame::Process", reinterpret_cast<void *>(CGame__Process),
	                      reinterpret_cast<void *>(&HookedProcess))) {
		Log("frame: FAILED to hook CGame::Process at 0x%08X", CGame__Process);
		for (const auto &f : HookFailures())
			Log("frame:   %s: %s", f.name.c_str(), f.reason.c_str());
		return false;
	}

	Log("frame: hooked CGame::Process at 0x%08X", CGame__Process);
	return true;
}

void RemoveFrameHook() {
	if (!g_detour.IsInstalled())
		return;
	g_detour.Remove();
	g_pre  = nullptr;
	g_post = nullptr;
	Log("frame: hook removed after %u frames", g_frames.load());
}

bool FrameHookInstalled() {
	return g_detour.IsInstalled();
}

uint32_t FramesSeen() {
	return g_frames.load(std::memory_order_relaxed);
}

bool WaitForGameLoop(uint32_t timeoutMs) {
#ifdef _WIN32
	// CTimer::Update does m_FrameCounter++ once per frame (re3 Timer.cpp:162),
	// so a change here means the game loop is genuinely running, which in
	// turn means every other ASI has finished patching. Waits for a *change*
	// rather than a non-zero value, because the counter resets to 0 on new
	// game / load (Timer.cpp:71), and 0 is also its .bss initial value.
	const volatile uint32_t *counter =
	    reinterpret_cast<const volatile uint32_t *>(CTimer__m_FrameCounter);

	// ---- UNRESOLVED: does this counter advance in the frontend? -----------
	//
	// Don't "fix" either side of this without new evidence. Two honest
	// readings disagree here, and this function's behaviour depends on
	// which one is right.
	//
	// MEASURED, on the live game with the real mod stack, twice: it does
	// NOT. Once with a 60 s timeout that expired with the main menu visible
	// and "START GAME" highlighted, once with a 20 minute timeout that
	// waited 115 s and only returned when the player loaded a save. This
	// function returns on any single change, so that's a very low bar, and
	// it still wasn't met.
	//
	// STATIC, from the retail image, says it should: WinMain's GS_FRONTEND
	// case (0x00582E23) calls RsEventHandler(rsFRONTENDIDLE) every frame the
	// window isn't minimised -> AppEventHandler (0x0048E800) -> FrontendIdle
	// (0x0048E700), which opens with `call CTimer::Update` (0x004ACF70); and
	// CTimer::Update ends at 0x004AD2F1 with an unconditional
	// `inc dword [0x009412EC]` that both arms of the preceding branch fall
	// through to. 0x009412EC's identity as m_FrameCounter is solid on its
	// own: CTimer::Initialise zeroes it at 0x004ACF36, and a dozen sites read
	// it as `movzx eax,[ebx+5Ah] / add eax,[9412ECh]`, which is re3's
	// `CTimer::GetFrameCounter() + m_randomSeed`.
	//
	// Leading hypothesis: something in the installed stack patches this path
	// at runtime - Framerate Vigilante exists to control frame pacing, which
	// means owning exactly this code - so both readings could be correct
	// about different binaries. That's the situation docs/compat.md §2.2
	// exists for. Untested.
	//
	// CoopIII is unaffected either way - it wants a running game, and the
	// measured behaviour gives it one. agentpad/ can't live with the
	// ambiguity, since it has to work on the menu, so it uses gGameState
	// (0x008F5838) instead, which doesn't go through CTimer at all, and logs
	// both signals side by side so the next in-game run settles this for
	// free. See agentpad/src/dllmain.cpp.
	//
	// What's NOT in doubt: CGame::Process is not called in the frontend. An
	// earlier version of this comment asserted that and the frame-counter
	// claim as one fact. They're two separate facts.
	const uint32_t start = *counter;
	const DWORD    began = GetTickCount();
	DWORD          lastReport = began;

	while (GetTickCount() - began < timeoutMs) {
		if (*counter != start)
			return true;

		const DWORD now = GetTickCount();
		if (now - lastReport >= 30000) {
			lastReport = now;
			Log("waiting for a game to start (%lu s so far). This is normal "
			    "while you are on the menu",
			    (now - began) / 1000);
		}
		Sleep(100);
	}
	return false;
#else
	(void)timeoutMs;
	return false;
#endif
}

} // namespace coopiii::game
