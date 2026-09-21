#include "inject.h"

#include "channel.h"
#include "protocol.h"

#include "game/addresses.h"
#include "hook/hook.h"
#include "log.h"

#include <atomic>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace agentpad {

namespace {

using namespace coopiii;
namespace ga = coopiii::game;

// protocol.cpp keeps its own copy of the CKeyboardState offsets so it can be
// unit-tested without the game headers. This file is the only place that sees
// both copies, so it's where they get pinned together. Edit one table without
// the other and the build breaks right here.
static_assert(0x226 == ga::pad::KEY_UP, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x228 == ga::pad::KEY_DOWN, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x22A == ga::pad::KEY_LEFT, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x22C == ga::pad::KEY_RIGHT, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x218 == ga::pad::KEY_ESC, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x256 == ga::pad::KEY_TAB, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x254 == ga::pad::KEY_BACKSP, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x222 == ga::pad::KEY_PGUP, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x224 == ga::pad::KEY_PGDN, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x21E == ga::pad::KEY_HOME, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x220 == ga::pad::KEY_END, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x21C == ga::pad::KEY_DEL, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(0x21A == ga::pad::KEY_INS, "protocol.cpp KEY_TABLE drifted from addresses.h");
static_assert(sizeof(Shared::pad) == ga::pad::SIZEOF_CONTROLLERSTATE,
              "Shared::pad must mirror CControllerState byte for byte");

// intro.cpp spells the two movie states as bare numbers so padtest can walk
// the state machine without the game headers. Same deal as above: pinned
// together here.
static_assert(ga::GS_LOGO_MPEG == 2, "ShouldSkipIntroFrom hardcodes GS_LOGO_MPEG == 2");
static_assert(ga::GS_INTRO_MPEG == 4, "ShouldSkipIntroFrom hardcodes GS_INTRO_MPEG == 4");
static_assert(ga::GS_INIT_ONCE == 5, "the intro skip jumps to GS_INIT_ONCE == 5");

Detour                g_detour;
Channel               g_channel;
InstanceIndex         g_index;
uint32_t              g_pid = 0;
std::atomic<uint32_t> g_frames{0};

// Decision logic. No game code in it at all, unit-tested standalone. Only
// ever touched from the game thread, inside the hook.
Engine   g_engine;
uint32_t g_lastStatus = STATUS_UNKNOWN;

// CPad::UpdatePads is a static member, no args, and the retail function ends
// in a bare `ret`, so it's __cdecl. MinHook's trampoline needs to be called
// with the same convention or the stack walks off.
using UpdatePadsFn = void(__cdecl *)();

inline uint8_t *PadZero() {
	return reinterpret_cast<uint8_t *>(ga::CPad__Pads);
}

void WriteControllerState(const int16_t *fields) {
	// CPad::Pads[0].NewState. NewState sits at offset 0 of CPad, so this is
	// really just the pad's own address - kept the +NEWSTATE for clarity.
	auto *dst = reinterpret_cast<int16_t *>(PadZero() + ga::pad::NEWSTATE);
	std::memcpy(dst, fields, ga::pad::SIZEOF_CONTROLLERSTATE);
}

void WriteKeyboardState(uint32_t keys) {
	auto *base = reinterpret_cast<uint8_t *>(ga::CPad__NewKeyState);

	// Only clear the members we own. Wiping the whole 0x270-byte struct would
	// take VK_KEYS with it, and that's how the game's own key bindings reach
	// the pad - a human sharing the session would lose every bound key, not
	// just the sixteen the driver knows about.
	auto put = [base](size_t offset, int16_t value) {
		*reinterpret_cast<int16_t *>(base + offset) = value;
	};

	const int16_t on = 1;   // the game only ever tests these against zero

	put(ga::pad::KEY_UP,     (keys & KEY_UP)     ? on : 0);
	put(ga::pad::KEY_DOWN,   (keys & KEY_DOWN)   ? on : 0);
	put(ga::pad::KEY_LEFT,   (keys & KEY_LEFT)   ? on : 0);
	put(ga::pad::KEY_RIGHT,  (keys & KEY_RIGHT)  ? on : 0);
	put(ga::pad::KEY_ESC,    (keys & KEY_ESC)    ? on : 0);
	put(ga::pad::KEY_TAB,    (keys & KEY_TAB)    ? on : 0);
	put(ga::pad::KEY_BACKSP, (keys & KEY_BACKSP) ? on : 0);
	put(ga::pad::KEY_PGUP,   (keys & KEY_PGUP)   ? on : 0);
	put(ga::pad::KEY_PGDN,   (keys & KEY_PGDN)   ? on : 0);
	put(ga::pad::KEY_HOME,   (keys & KEY_HOME)   ? on : 0);
	put(ga::pad::KEY_END,    (keys & KEY_END)    ? on : 0);
	put(ga::pad::KEY_DEL,    (keys & KEY_DEL)    ? on : 0);
	put(ga::pad::KEY_INS,    (keys & KEY_INS)    ? on : 0);

	// "Enter" maps to two members. CPad::GetEnter() reads EXTENTER (main
	// Return key), GetPadEnter() reads ENTER (keypad), and the frontend
	// accepts either - so a driver asking for "Enter" gets both set. Same
	// deal for Shift: the game tracks LSHIFT plus a combined SHIFT.
	const int16_t enter = (keys & KEY_ENTER) ? on : 0;
	put(ga::pad::KEY_ENTER,    enter);
	put(ga::pad::KEY_EXTENTER, enter);

	const int16_t shift = (keys & KEY_SHIFT) ? on : 0;
	put(ga::pad::KEY_LSHIFT, shift);
	put(ga::pad::KEY_SHIFT,  shift);

	// Space lives in VK_KEYS, which is indexed by character code.
	auto *vk = reinterpret_cast<int16_t *>(base + ga::pad::KEY_VK_KEYS);
	vk[' '] = (keys & KEY_SPACE) ? on : 0;
}

void WriteMouseDelta(float dx, float dy) {
	auto *base = reinterpret_cast<uint8_t *>(ga::CPad__NewMouseControllerState);
	*reinterpret_cast<float *>(base + ga::pad::MOUSE_X) = dx;
	*reinterpret_cast<float *>(base + ga::pad::MOUSE_Y) = dy;
}

void __cdecl HookedUpdatePads() {
	// Original always runs first. CPad::Update composes NewState from the
	// three PCTemp* states and clears them, and UpdatePads writes NewKeyState
	// as its last act - so everything we want to overwrite only finishes
	// once the original returns. Writing before that would just get clobbered
	// by values the game is about to recompute.
	g_detour.Original<UpdatePadsFn>()();

	g_frames.fetch_add(1, std::memory_order_relaxed);

	Shared *shm = g_channel.Get();
	if (!shm)
		return;   // no section, pure pass-through, human keeps the keyboard

#ifdef _WIN32
	const uint32_t now = GetTickCount();
#else
	const uint32_t now = 0;
#endif

	// Publish which part of the game is running so a driver can tell whether
	// to send menu keys or gameplay input, instead of guessing from timing.
	shm->modGameState = *reinterpret_cast<const volatile uint32_t *>(ga::gGameState);

	// Everything about *what* to press is decided in here, no game code
	// involved. This function just copies the answer into CPad.
	const Engine::Frame f = g_engine.Step(*shm, now);

	if (f.status != g_lastStatus) {
		g_lastStatus = f.status;
		Log("agentpad: status -> %s",
		    f.status == STATUS_INJECTING     ? "injecting"
		    : f.status == STATUS_PASSTHROUGH ? "pass-through"
		    : f.status == STATUS_STALE       ? "stale (driver stopped refreshing; released)"
		    : f.status == STATUS_BAD_HEADER  ? "bad header (no driver has claimed the section)"
		                                     : "unknown");
	}

	if (f.channels & CHANNEL_PAD)
		WriteControllerState(f.pad);

	if (f.channels & CHANNEL_KEYS)
		WriteKeyboardState(f.keys);

	// Written every frame, zero included. These are deltas - leave a stale
	// non-zero value sitting there and the camera keeps turning.
	if (f.channels & CHANNEL_MOUSE)
		WriteMouseDelta(static_cast<float>(f.mouseDx), static_cast<float>(f.mouseDy));
}

} // namespace

bool InstallPadHook() {
#ifdef _WIN32
	g_pid = GetCurrentProcessId();
#endif

	if (!g_channel.Create(g_pid)) {
		// Not fatal. No section means nothing can drive the game yet, but the
		// hook's still worth installing - a driver might show up later, and
		// bailing here would be exactly the silent-failure case docs/compat.md
		// §2.5 warns against.
		Log("agentpad: could not publish shared memory for pid %u: %s", g_pid,
		    g_channel.Error());
	} else {
		Log("agentpad: shared memory \"%s\" published, %u bytes", g_channel.Name(),
		    static_cast<unsigned>(sizeof(Shared)));

		// Register in the instance directory so a driver can enumerate every
		// running game without knowing the executable name. Failing to
		// register isn't fatal - the section's still there, and a driver given
		// this pid directly can still open it.
		if (g_index.Open(true) && g_index.Add(g_pid)) {
			uint32_t pids[INDEX_SLOTS] = {0};
			const uint32_t n = g_index.List(pids, INDEX_SLOTS);
			Log("agentpad: registered pid %u in \"%s\" (%u instance(s) listed)", g_pid,
			    SHM_INDEX_NAME, n);
			if (n > 1)
				Log("agentpad: another game instance is already listed, so a driver must say "
				    "which one it means (Get-GtaInstance / -GamePid)");
		} else {
			Log("agentpad: could not register pid %u in the instance index: %s. Drivers will "
			    "have to be given this pid directly",
			    g_pid, g_index.Error());
		}
	}

	if (!g_detour.Install("CPad::UpdatePads", reinterpret_cast<void *>(ga::CPad__UpdatePads),
	                      reinterpret_cast<void *>(&HookedUpdatePads))) {
		Log("agentpad: FAILED to hook CPad::UpdatePads at 0x%08X", ga::CPad__UpdatePads);
		for (const auto &f : HookFailures())
			Log("agentpad:   %s: %s", f.name.c_str(), f.reason.c_str());
		g_index.Remove(g_pid);
		g_index.Close();
		g_channel.Close();
		return false;
	}

	if (Shared *shm = g_channel.Get()) {
		shm->modMagic  = MAGIC;
		shm->modStatus = STATUS_UNKNOWN;
		shm->modPid    = g_pid;
	}

	Log("agentpad: hooked CPad::UpdatePads at 0x%08X (pads at 0x%08X)", ga::CPad__UpdatePads,
	    ga::CPad__Pads);
	return true;
}

void RemovePadHook() {
	if (g_detour.IsInstalled()) {
		g_detour.Remove();
		Log("agentpad: hook removed after %u frames, %u injected", g_frames.load(),
		    g_engine.Applied());
	}
	if (Shared *shm = g_channel.Get()) {
		shm->modMagic  = 0;
		shm->modStatus = STATUS_UNKNOWN;
	}
	// Leave the index before the section goes away. A driver reading the
	// table in between sees a pid whose section won't open - which is exactly
	// how it's meant to recognize a dead entry anyway.
	g_index.Remove(g_pid);
	g_index.Close();
	g_channel.Close();
}

bool PadHookInstalled() {
	return g_detour.IsInstalled();
}

uint32_t PadFramesSeen() {
	return g_frames.load(std::memory_order_relaxed);
}

} // namespace agentpad
