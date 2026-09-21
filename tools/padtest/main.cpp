// Unit tests for everything in AgentPad that doesn't need GTA III running:
// shared-memory layout and header validation, the torn-read rule, the
// watchdog, the mouse budget drain, the name tables, and a real named
// section created and opened in this process.
//
//   xmake build padtest && xmake run padtest
//
// Not covered here, because it can't be: the actual writes into CPad::Pads,
// CPad::NewKeyState and CPad::NewMouseControllerState. Those need the game's
// address space. Everything that decides *what* gets written is here though.

#include "protocol.h"
#include "channel.h"
#include "cdstream.h"
#include "intro.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace agentpad;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char *what) {
	++g_checks;
	if (!ok) {
		++g_failed;
		std::printf("  FAIL  %s\n", what);
	}
}

void CheckEq(long long got, long long want, const char *what) {
	++g_checks;
	if (got != want) {
		++g_failed;
		std::printf("  FAIL  %s: got %lld, want %lld\n", what, got, want);
	}
}

void Section(const char *name) {
	std::printf("\n%s\n", name);
}

uint32_t SelfPid() {
#ifdef _WIN32
	return GetCurrentProcessId();
#else
	return 0;
#endif
}

// Fills a Shared the way the driver is supposed to.
Shared MakeValid() {
	Shared s{};
	s.magic        = MAGIC;
	s.version      = VERSION;
	s.enabled      = 1;
	s.channels     = CHANNEL_ALL;
	s.writerTickMs = 1000;
	s.timeoutMs    = 0;
	s.seq          = 1;
	return s;
}

// ---------------------------------------------------------------------------

void TestLayout() {
	Section("layout - the contract the PowerShell driver pokes by offset");

	CheckEq(sizeof(Shared), 0x80, "sizeof(Shared)");
	CheckEq(offsetof(Shared, magic), 0x00, "magic");
	CheckEq(offsetof(Shared, version), 0x04, "version");
	CheckEq(offsetof(Shared, seq), 0x08, "seq");
	CheckEq(offsetof(Shared, enabled), 0x0C, "enabled");
	CheckEq(offsetof(Shared, channels), 0x10, "channels");
	CheckEq(offsetof(Shared, writerTickMs), 0x14, "writerTickMs");
	CheckEq(offsetof(Shared, timeoutMs), 0x18, "timeoutMs");
	CheckEq(offsetof(Shared, keys), 0x1C, "keys");
	CheckEq(offsetof(Shared, mouseBudgetX), 0x20, "mouseBudgetX");
	CheckEq(offsetof(Shared, mouseBudgetY), 0x24, "mouseBudgetY");
	CheckEq(offsetof(Shared, mouseSeq), 0x28, "mouseSeq");
	CheckEq(offsetof(Shared, mouseStepMax), 0x2C, "mouseStepMax");
	CheckEq(offsetof(Shared, modMagic), 0x30, "modMagic");
	CheckEq(offsetof(Shared, modPid), 0x34, "modPid");
	CheckEq(offsetof(Shared, modFrames), 0x38, "modFrames");
	CheckEq(offsetof(Shared, modApplied), 0x3C, "modApplied");
	CheckEq(offsetof(Shared, modSeqSeen), 0x40, "modSeqSeen");
	CheckEq(offsetof(Shared, modStatus), 0x44, "modStatus");
	CheckEq(offsetof(Shared, modMouseLeftX), 0x48, "modMouseLeftX");
	CheckEq(offsetof(Shared, modMouseLeftY), 0x4C, "modMouseLeftY");
	CheckEq(offsetof(Shared, pad), 0x50, "pad[]");
	CheckEq(offsetof(Shared, modGameState), 0x7C, "modGameState");

	// pad[] is a byte-for-byte mirror of the game's CControllerState: 21
	// int16, no padding. Checked against the retail binary - CPad::Update
	// copies exactly these 21 fields from NewState to OldState.
	CheckEq(sizeof(Shared::pad), 0x2A, "sizeof(pad[]) == sizeof(CControllerState)");
	CheckEq(sizeof(Shared{}.pad) / sizeof(int16_t), 21, "pad[] has 21 fields");

	// 'APAD' as little-endian ASCII so a hex dump of the section reads clean.
	const char *m = reinterpret_cast<const char *>(&MAGIC);
	Check(m[0] == 'A' && m[1] == 'P' && m[2] == 'A' && m[3] == 'D', "magic spells APAD");
}

void TestHeaderValidation() {
	Section("header validation - a section nobody has claimed must not be obeyed");

	Decision none;

	// A zero-filled section is what CreateFileMapping hands back. Must read
	// as "no driver", not as "inject 21 zeroed fields".
	Shared zeroed{};
	Decision d = Decide(zeroed, 0, none);
	CheckEq(d.status, STATUS_BAD_HEADER, "zero-filled section -> BAD_HEADER");
	Check(!d.Injecting(), "zero-filled section does not inject");
	CheckEq(d.channels, 0, "BAD_HEADER touches no channel");

	Shared wrongMagic = MakeValid();
	wrongMagic.magic  = 0xDEADBEEF;
	d                 = Decide(wrongMagic, 1000, none);
	CheckEq(d.status, STATUS_BAD_HEADER, "wrong magic -> BAD_HEADER");
	Check(!d.Injecting(), "wrong magic does not inject");

	Shared wrongVersion  = MakeValid();
	wrongVersion.version = VERSION + 1;
	d                    = Decide(wrongVersion, 1000, none);
	CheckEq(d.status, STATUS_BAD_HEADER, "wrong version -> BAD_HEADER");

	// A future driver must never be able to drive an older mod by accident.
	Check(!Decide(wrongVersion, 1000, none).Injecting(), "version mismatch never injects");
}

void TestEnableDisable() {
	Section("enable / disable - pass-through is the default and the fallback");

	Decision none;

	Shared off = MakeValid();
	off.enabled = 0;
	Decision d  = Decide(off, 1000, none);
	CheckEq(d.status, STATUS_PASSTHROUGH, "enabled=0 -> PASSTHROUGH");
	Check(!d.Injecting(), "disabled does not inject");
	CheckEq(d.channels, 0, "disabled clears the channel mask");

	Shared on = MakeValid();
	on.pad[IDX_LEFT_STICK_Y] = -128;
	d = Decide(on, 1000, none);
	CheckEq(d.status, STATUS_INJECTING, "enabled=1 -> INJECTING");
	Check(d.Injecting(), "enabled injects");
	CheckEq(d.pad[IDX_LEFT_STICK_Y], -128, "the controller state survives the copy");

	// enabled with no channels selected is a no-op, not an injection.
	Shared noChannels   = MakeValid();
	noChannels.channels = 0;
	d                   = Decide(noChannels, 1000, none);
	Check(!d.Injecting(), "enabled with no channels injects nothing");

	// Unknown channel bits are masked off rather than passed through.
	Shared junkChannels   = MakeValid();
	junkChannels.channels = 0xFFFFFFFFu;
	d                     = Decide(junkChannels, 1000, none);
	CheckEq(d.channels, CHANNEL_ALL, "unknown channel bits are masked off");
}

void TestChannels() {
	Section("channels - the three input surfaces are independently selectable");

	Decision none;

	Shared s   = MakeValid();
	s.channels = CHANNEL_KEYS;
	Decision d = Decide(s, 1000, none);
	Check(d.Injecting(), "keys-only still counts as injecting");
	CheckEq(d.channels & CHANNEL_PAD, 0, "keys-only leaves the pad alone");
	CheckEq(d.channels & CHANNEL_MOUSE, 0, "keys-only leaves the mouse alone");
	Check((d.channels & CHANNEL_KEYS) != 0, "keys-only selects the keyboard");

	s.channels = CHANNEL_PAD | CHANNEL_MOUSE;
	d          = Decide(s, 1000, none);
	CheckEq(d.channels & CHANNEL_KEYS, 0, "pad+mouse leaves the keyboard alone");

	CheckEq(CHANNEL_ALL, CHANNEL_PAD | CHANNEL_KEYS | CHANNEL_MOUSE, "CHANNEL_ALL is all three");
}

void TestWatchdog() {
	Section("watchdog - an abandoned hold must release, not run forever");

	Decision none;

	Shared s      = MakeValid();
	s.writerTickMs = 10000;
	s.timeoutMs    = 500;

	Decision d = Decide(s, 10000, none);
	CheckEq(d.status, STATUS_INJECTING, "fresh heartbeat injects");

	d = Decide(s, 10499, none);
	CheckEq(d.status, STATUS_INJECTING, "just inside the timeout still injects");

	d = Decide(s, 10500, none);
	CheckEq(d.status, STATUS_INJECTING, "exactly at the timeout still injects");

	d = Decide(s, 10501, none);
	CheckEq(d.status, STATUS_STALE, "past the timeout goes stale");
	Check(!d.Injecting(), "stale releases the input");
	CheckEq(d.channels, 0, "stale touches no channel");

	// timeoutMs == 0 means the driver has opted out of supervision.
	s.timeoutMs = 0;
	d           = Decide(s, 10000 + 60 * 60 * 1000, none);
	CheckEq(d.status, STATUS_INJECTING, "timeout 0 never goes stale");

	// GetTickCount wraps every 49.7 days. Unsigned subtraction has to carry
	// across that wrap, or a session started just before it would have its
	// input released at a random moment.
	s.timeoutMs    = 1000;
	s.writerTickMs = 0xFFFFFF00u;
	d              = Decide(s, 0x00000064u, none);   // 0x164 ticks later
	CheckEq(d.status, STATUS_INJECTING, "heartbeat survives the tick-count rollover");

	d = Decide(s, 0x00000500u, none);                // 0x600 ticks later
	CheckEq(d.status, STATUS_STALE, "the watchdog still fires across the rollover");
}

void TestTornRead() {
	Section("torn reads - a mixed controller state is a stuck button");

	// Stands in for a driver caught mid-write: Decide reads seq, copies,
	// reads seq again. We can't interleave a real writer here, so instead we
	// model the observable consequence - seq differing across the copy.
	Decision good;
	good.status   = STATUS_INJECTING;
	good.channels = CHANNEL_PAD;
	good.seq      = 7;
	good.pad[IDX_CROSS] = 255;

	// A struct whose seq is read twice from the same memory can't differ, so
	// this case gets exercised through the contract instead: an untorn read
	// of a valid section yields that section, and lastGood only comes back
	// when the header is good. Prove the non-torn path first.
	Shared s = MakeValid();
	s.seq    = 9;
	s.pad[IDX_CROSS] = 255;
	Decision d = Decide(s, 1000, good);
	CheckEq(d.seq, 9, "an untorn read takes the new generation");
	CheckEq(d.pad[IDX_CROSS], 255, "an untorn read takes the new state");

	// And prove the fallback value is carried, not made up: a bad header must
	// yield a *fresh* refusal rather than the previous good state - otherwise
	// a driver that exits and unmaps would leave its last frame latched.
	Shared dead{};
	d = Decide(dead, 1000, good);
	CheckEq(d.status, STATUS_BAD_HEADER, "a vanished driver refuses, it does not latch");
	CheckEq(d.pad[IDX_CROSS], 0, "a vanished driver does not leave the button held");
}

void TestMouseDrain() {
	Section("mouse budget - the same command must turn the same amount at any frame rate");

	// stepMax 0 means "all in one frame".
	CheckEq(DrainStep(900, 0), 900, "stepMax 0 delivers the whole budget");
	CheckEq(DrainStep(-900, 0), -900, "stepMax 0 works for negatives too");
	CheckEq(DrainStep(0, 0), 0, "an empty budget delivers nothing");
	CheckEq(DrainStep(0, 30), 0, "an empty budget delivers nothing with a cap either");

	CheckEq(DrainStep(900, 30), 30, "a capped budget delivers the cap");
	CheckEq(DrainStep(20, 30), 20, "a budget under the cap delivers all of it");
	CheckEq(DrainStep(30, 30), 30, "a budget exactly at the cap delivers all of it");

	CheckEq(DrainStep(-900, 30), -30, "a negative budget steps negatively");
	CheckEq(DrainStep(-20, 30), -20, "a small negative budget delivers all of it");

	// Never overshoot, never flip sign - the loop below has to terminate at
	// exactly the requested total. That's the entire point of a budget.
	int32_t  left  = 900;
	int32_t  total = 0;
	int      frames = 0;
	while (left != 0 && frames < 1000) {
		const int32_t step = DrainStep(left, 30);
		Check(step != 0, "a non-empty budget always makes progress");
		total += step;
		left -= step;
		++frames;
	}
	CheckEq(total, 900, "draining delivers exactly the requested total");
	CheckEq(frames, 30, "900 at 30/frame takes 30 frames");
	CheckEq(left, 0, "the budget lands exactly on zero");

	// A total that isn't a multiple of the cap still has to land exactly.
	left = -95; total = 0; frames = 0;
	while (left != 0 && frames < 1000) {
		const int32_t step = DrainStep(left, 30);
		total += step;
		left -= step;
		++frames;
	}
	CheckEq(total, -95, "a ragged negative total lands exactly");
	CheckEq(frames, 4, "-95 at 30/frame takes 4 frames");

	// INT32_MIN is the one value where negating the budget would overflow.
	const int32_t intMin = (-2147483647 - 1);
	CheckEq(DrainStep(intMin, 30), -30, "INT32_MIN budget does not overflow");
	CheckEq(DrainStep(intMin, 0), intMin, "INT32_MIN with no cap is passed through");

	// A cap larger than INT32_MAX has to mean "no cap", not a sign flip.
	CheckEq(DrainStep(900, 0xFFFFFFFFu), 900, "an enormous cap delivers everything");
}

void TestMouseBudgetPlumbing() {
	Section("mouse budget - reaching the hook intact");

	Decision none;
	Shared   s      = MakeValid();
	s.mouseBudgetX  = 900;
	s.mouseBudgetY  = -45;
	s.mouseSeq      = 3;
	s.mouseStepMax  = 30;

	Decision d = Decide(s, 1000, none);
	CheckEq(d.mouseBudgetX, 900, "budget X survives");
	CheckEq(d.mouseBudgetY, -45, "budget Y survives");
	CheckEq(d.mouseSeq, 3, "mouse seq survives");
	CheckEq(d.mouseStepMax, 30, "step cap survives");

	// Disabled must not carry a budget through - the hook cancels in-flight
	// pans when it stops injecting, and needs a zeroed decision to do that.
	s.enabled = 0;
	d         = Decide(s, 1000, none);
	CheckEq(d.mouseBudgetX, 0, "a disabled driver carries no budget");
	CheckEq(d.mouseSeq, 0, "a disabled driver carries no mouse seq");
}

void TestEngine() {
	Section("the per-frame engine - the exact code the hook runs");

	// A camera pan latches once and drains over frames. That's the property
	// that makes "turn 90 degrees" mean the same thing at 30 fps as at 60,
	// so it's worth testing as a sequence rather than a single call.
	{
		Shared s        = MakeValid();
		s.channels      = CHANNEL_MOUSE;
		s.mouseBudgetX  = 900;
		s.mouseBudgetY  = 0;
		s.mouseStepMax  = 30;
		s.mouseSeq      = 1;

		Engine  e;
		int32_t total  = 0;
		int     frames = 0;
		for (int i = 0; i < 100; ++i) {
			const Engine::Frame f = e.Step(s, 1000);
			total += f.mouseDx;
			if (f.mouseDx != 0)
				++frames;
		}
		CheckEq(total, 900, "the engine delivers exactly the requested pan");
		CheckEq(frames, 30, "900 at 30/frame takes 30 frames");
		CheckEq(e.MouseRemainingX(), 0, "the budget ends empty");
		CheckEq(s.modMouseLeftX, 0, "the remaining budget is published back");
		CheckEq(s.modFrames, 100, "every frame is counted");
		CheckEq(s.modApplied, 100, "every injected frame is counted");
	}

	// The same budget must not be re-delivered just because it's still sitting
	// in the section. Only a new mouseSeq re-latches it.
	{
		Shared s       = MakeValid();
		s.channels     = CHANNEL_MOUSE;
		s.mouseBudgetX = 100;
		s.mouseStepMax = 0;   // all in one frame
		s.mouseSeq     = 1;

		Engine e;
		CheckEq(e.Step(s, 0).mouseDx, 100, "the first frame delivers the budget");
		CheckEq(e.Step(s, 0).mouseDx, 0, "an unchanged seq does not re-deliver it");
		CheckEq(e.Step(s, 0).mouseDx, 0, "and still does not");

		s.mouseSeq = 2;
		CheckEq(e.Step(s, 0).mouseDx, 100, "a new seq latches the budget again");
	}

	// Publishing zero with a fresh seq cancels a pan mid-flight.
	{
		Shared s       = MakeValid();
		s.channels     = CHANNEL_MOUSE;
		s.mouseBudgetX = 900;
		s.mouseStepMax = 30;
		s.mouseSeq     = 1;

		Engine e;
		e.Step(s, 0);
		CheckEq(e.MouseRemainingX(), 870, "the pan is in flight");

		s.mouseBudgetX = 0;
		s.mouseSeq     = 2;
		const Engine::Frame f = e.Step(s, 0);
		CheckEq(f.mouseDx, 0, "cancelling delivers nothing");
		CheckEq(e.MouseRemainingX(), 0, "cancelling empties the budget");
	}

	// Disabling injection mid-pan must cancel it, not freeze it. The game
	// only recomputes NewMouseControllerState when the window has focus, so
	// a leftover delta would get re-read every frame and spin the camera
	// forever.
	{
		Shared s       = MakeValid();
		s.channels     = CHANNEL_MOUSE;
		s.mouseBudgetX = 900;
		s.mouseStepMax = 30;
		s.mouseSeq     = 1;

		Engine e;
		e.Step(s, 0);
		Check(e.MouseRemainingX() != 0, "a pan is in flight");

		s.enabled = 0;
		const Engine::Frame f = e.Step(s, 0);
		CheckEq(f.status, STATUS_PASSTHROUGH, "disabling reports pass-through");
		CheckEq(f.channels, 0, "disabling writes nothing");
		CheckEq(e.MouseRemainingX(), 0, "disabling cancels the pan in flight");
		CheckEq(s.modMouseLeftX, 0, "the cancellation is published back");

		// And re-enabling must not resurrect it.
		s.enabled = 1;
		CheckEq(e.Step(s, 0).mouseDx, 0, "re-enabling does not resurrect a cancelled pan");
	}

	// The watchdog firing must release just as thoroughly.
	{
		Shared s       = MakeValid();
		s.channels     = CHANNEL_MOUSE | CHANNEL_PAD;
		s.mouseBudgetX = 900;
		s.mouseStepMax = 30;
		s.mouseSeq     = 1;
		s.writerTickMs = 0;
		s.timeoutMs    = 100;
		s.pad[IDX_LEFT_STICK_Y] = -128;

		Engine e;
		Engine::Frame f = e.Step(s, 50);
		CheckEq(f.status, STATUS_INJECTING, "inside the timeout it injects");
		CheckEq(f.pad[IDX_LEFT_STICK_Y], -128, "and the walk is applied");

		f = e.Step(s, 5000);
		CheckEq(f.status, STATUS_STALE, "past the timeout it goes stale");
		CheckEq(f.channels, 0, "a stale driver writes nothing at all");
		CheckEq(e.MouseRemainingX(), 0, "a stale driver's pan is cancelled");
	}

	// Frame counting must include frames where nothing was injected, or a
	// script can't tell "the mod is dead" from "the mod is ignoring me".
	{
		Shared s  = MakeValid();
		s.enabled = 0;

		Engine e;
		for (int i = 0; i < 5; ++i)
			e.Step(s, 0);
		CheckEq(e.Frames(), 5, "pass-through frames are still counted");
		CheckEq(e.Applied(), 0, "but not counted as applied");
		CheckEq(s.modFrames, 5, "the frame count is published back");
		CheckEq(s.modStatus, STATUS_PASSTHROUGH, "the status is published back");
	}

	// An unclaimed section must be counted too - that's the state the mod
	// sits in from load until a driver shows up.
	{
		Shared zeroed{};
		Engine e;
		const Engine::Frame f = e.Step(zeroed, 0);
		CheckEq(f.status, STATUS_BAD_HEADER, "an unclaimed section reports BAD_HEADER");
		CheckEq(f.channels, 0, "an unclaimed section is not obeyed");
		CheckEq(e.Frames(), 1, "an unclaimed section still counts frames");
		CheckEq(zeroed.modFrames, 1, "and still publishes them");
	}
}

void TestKeyTable() {
	Section("key names and offsets - the driver and the mod must agree on spelling");

	// Every bit is distinct and none is zero, otherwise two names would collide.
	uint32_t seen = 0;
	for (Key k : ALL_KEYS) {
		Check(k != 0, "no key bit is zero");
		Check((seen & k) == 0, "key bits do not collide");
		seen |= k;
	}
	CheckEq(seen, KEY_ALL, "ALL_KEYS covers exactly KEY_ALL");

	// Round trip through the name table.
	for (Key k : ALL_KEYS) {
		const char *name = KeyName(k);
		Check(name[0] != '\0', "every key bit has a name");
		CheckEq(KeyFromName(name), k, "name round-trips to the same bit");
	}

	// Case insensitive, since nobody types "PageDown" consistently.
	CheckEq(KeyFromName("up"), KEY_UP, "lowercase name resolves");
	CheckEq(KeyFromName("UP"), KEY_UP, "uppercase name resolves");
	CheckEq(KeyFromName("Escape"), KEY_ESC, "Escape resolves");
	CheckEq(KeyFromName("esc"), KEY_ESC, "the Esc alias resolves");
	CheckEq(KeyFromName("Back"), KEY_ESC, "the Back alias resolves");
	CheckEq(KeyFromName("Return"), KEY_ENTER, "the Return alias resolves");

	// An unknown name must resolve to zero, never a guess. A driver that
	// typos a button name should press nothing, not something else.
	CheckEq(KeyFromName("Banana"), 0, "an unknown key name resolves to nothing");
	CheckEq(KeyFromName(""), 0, "an empty key name resolves to nothing");
	CheckEq(KeyFromName(nullptr), 0, "a null key name resolves to nothing");

	// Offsets, against the CKeyboardState layout verified in the binary:
	// F[12] at 0x00, VK_KEYS[256] at 0x18, then the named keys from 0x218.
	size_t off = 0;
	Check(KeyFieldOffset(KEY_UP, &off), "UP has an offset");
	CheckEq(off, 0x226, "UP is at CKeyboardState+0x226");
	Check(KeyFieldOffset(KEY_DOWN, &off), "DOWN has an offset");
	CheckEq(off, 0x228, "DOWN is at CKeyboardState+0x228");
	Check(KeyFieldOffset(KEY_ESC, &off), "ESC has an offset");
	CheckEq(off, 0x218, "ESC is the first named key, at +0x218");
	Check(KeyFieldOffset(KEY_TAB, &off), "TAB has an offset");
	CheckEq(off, 0x256, "TAB is at CKeyboardState+0x256");

	// These three are not single fields, on purpose.
	Check(!KeyFieldOffset(KEY_ENTER, &off), "ENTER is two fields, not one");
	Check(!KeyFieldOffset(KEY_SHIFT, &off), "SHIFT is two fields, not one");
	Check(!KeyFieldOffset(KEY_SPACE, &off), "SPACE lives inside VK_KEYS");

	// Every single-field offset must land inside the struct and be 2-aligned.
	for (Key k : ALL_KEYS) {
		if (!KeyFieldOffset(k, &off))
			continue;
		Check(off + 2 <= 0x270, "key offset lies inside CKeyboardState");
		Check(off % 2 == 0, "key offset is int16-aligned");
		Check(off >= 0x218, "named keys start after VK_KEYS");
	}
}

void TestPadTable() {
	Section("controller field names - index order is re3's declaration order");

	for (int i = 0; i < 21; ++i) {
		const char *name = PadFieldName(i);
		Check(name[0] != '\0', "every controller field has a name");
		CheckEq(PadFieldFromName(name), i, "controller name round-trips to the same index");
	}

	// The indices the rest of the system depends on.
	CheckEq(PadFieldFromName("LeftStickX"), 0, "LeftStickX is field 0");
	CheckEq(PadFieldFromName("LeftStickY"), 1, "LeftStickY is field 1");
	CheckEq(PadFieldFromName("RightStickX"), 2, "RightStickX is field 2");
	CheckEq(PadFieldFromName("RightStickY"), 3, "RightStickY is field 3");
	CheckEq(PadFieldFromName("DPadUp"), 8, "DPadUp is field 8");
	CheckEq(PadFieldFromName("DPadDown"), 9, "DPadDown is field 9");
	CheckEq(PadFieldFromName("DPadLeft"), 10, "DPadLeft is field 10");
	CheckEq(PadFieldFromName("DPadRight"), 11, "DPadRight is field 11");
	CheckEq(PadFieldFromName("Cross"), 16, "Cross is field 16, GTA III's sprint");
	CheckEq(PadFieldFromName("NetworkTalk"), 20, "NetworkTalk is the last field");

	// Shoulder aliases.
	CheckEq(PadFieldFromName("L1"), PadFieldFromName("LeftShoulder1"), "L1 aliases LeftShoulder1");
	CheckEq(PadFieldFromName("L2"), PadFieldFromName("LeftShoulder2"), "L2 aliases LeftShoulder2");
	CheckEq(PadFieldFromName("R1"), PadFieldFromName("RightShoulder1"), "R1 aliases RightShoulder1");
	CheckEq(PadFieldFromName("R2"), PadFieldFromName("RightShoulder2"), "R2 aliases RightShoulder2");

	CheckEq(PadFieldFromName("cross"), 16, "controller names are case-insensitive");
	CheckEq(PadFieldFromName("Banana"), -1, "an unknown controller name resolves to -1");
	CheckEq(PadFieldFromName(nullptr), -1, "a null controller name resolves to -1");
	CheckEq(PadFieldName(-1)[0], '\0', "an out-of-range index has no name");
	CheckEq(PadFieldName(21)[0], '\0', "an out-of-range index has no name");

	// 255 isn't a made-up constant. It's what the game's own PC input layer
	// writes into PCTempJoyState for a pressed digital button.
	CheckEq(BUTTON_DOWN, 255, "a pressed button is 255, matching the game's own input layer");
	CheckEq(AXIS_MAX, 32767, "full analog deflection matches GetLeftStickX's divisor");
}

void TestSectionNaming() {
	Section("section naming - one name per game process, never a truncated one");

	char name[SHM_NAME_MAX];

	Check(FormatSectionName(4312, name, sizeof(name)), "a name is produced");
	Check(std::strcmp(name, "CoopIII.AgentPad.v1.4312") == 0, "prefix, dot, decimal pid");

	Check(FormatSectionName(0, name, sizeof(name)), "pid 0 still formats");
	Check(std::strcmp(name, "CoopIII.AgentPad.v1.0") == 0, "pid 0 is \"0\", not empty");

	Check(FormatSectionName(0xFFFFFFFFu, name, sizeof(name)), "the widest pid fits");
	Check(std::strcmp(name, "CoopIII.AgentPad.v1.4294967295") == 0, "and is right");

	// Two pids must never share a name, and a name must never be silently
	// shortened - a truncated name is still a different *valid* name, and
	// two games could end up sharing it.
	char a[SHM_NAME_MAX], b[SHM_NAME_MAX];
	FormatSectionName(12, a, sizeof(a));
	FormatSectionName(123, b, sizeof(b));
	Check(std::strcmp(a, b) != 0, "different pids get different names");

	char tiny[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
	Check(!FormatSectionName(4312, tiny, sizeof(tiny)), "a buffer that cannot hold it fails");
	Check(tiny[0] == 'x', "and writes nothing at all rather than a truncated name");
	Check(!FormatSectionName(1, nullptr, 40), "a null buffer fails");
	Check(!FormatSectionName(1, name, 0), "a zero-length buffer fails");

	// The index keeps the old fixed name on purpose - it's the one thing
	// that has to be findable without already knowing a pid.
	Check(std::strcmp(SHM_INDEX_NAME, "CoopIII.AgentPad.v1.index") == 0,
	      "the instance index has the fixed, discoverable name");
	CheckEq(sizeof(Index), 0x40, "sizeof(Index)");
	CheckEq(offsetof(Index, capacity), 0x08, "Index::capacity");
	CheckEq(offsetof(Index, pid), 0x10, "Index::pid[]");
	CheckEq(INDEX_SLOTS, 8, "eight instance slots");
}

void TestInstanceIndex() {
	Section("the instance index - how a driver finds every running game");

	InstanceIndex index;
	if (!index.Open(true)) {
		std::printf("  SKIP  cannot create the index here (%s)\n", index.Error());
		return;
	}
	Check(index.IsOpen(), "the index can be created");

	// Pids that can't collide with anything real - Windows pids are multiples
	// of 4, and nothing near the top of the range is in use.
	const uint32_t fakeA = 0xFFFF0001u;
	const uint32_t fakeB = 0xFFFF0002u;

	uint32_t before[INDEX_SLOTS] = {0};
	const uint32_t nBefore       = index.List(before, INDEX_SLOTS);

	Check(index.Add(fakeA), "a pid can claim a slot");
	Check(index.Add(fakeA), "claiming twice is success, not a second slot");

	uint32_t pids[INDEX_SLOTS] = {0};
	uint32_t n                 = index.List(pids, INDEX_SLOTS);
	CheckEq(n, nBefore + 1, "exactly one slot was taken");

	int seen = 0;
	for (uint32_t i = 0; i < n; ++i)
		if (pids[i] == fakeA)
			++seen;
	CheckEq(seen, 1, "the pid is listed exactly once");

	Check(index.Add(fakeB), "a second instance can register too");
	n = index.List(pids, INDEX_SLOTS);
	CheckEq(n, nBefore + 2, "two instances are listed");

	// A driver is expected to confirm every listed pid by opening its
	// section, since the table can outlive a game that crashed. Neither of
	// these two has one.
	Check(!InstanceIsLive(fakeA), "a listed pid with no section is not live");
	Check(!InstanceIsLive(fakeB), "nor is the other one");

	index.Remove(fakeA);
	index.Remove(fakeB);
	n = index.List(pids, INDEX_SLOTS);
	CheckEq(n, nBefore, "unregistering frees the slots again");

	index.Remove(fakeA);   // must be harmless
	CheckEq(index.List(pids, INDEX_SLOTS), nBefore, "removing a pid twice changes nothing");

	// A second opener sees the same table - that's the whole mechanism, and
	// it's exactly what a PowerShell driver does.
	Check(index.Add(fakeA), "re-register for the cross-process check");
	InstanceIndex reader;
	Check(reader.Open(false), "a reader can open the index without creating it");
	uint32_t fromReader[INDEX_SLOTS] = {0};
	const uint32_t m                 = reader.List(fromReader, INDEX_SLOTS);
	seen                             = 0;
	for (uint32_t i = 0; i < m; ++i)
		if (fromReader[i] == fakeA)
			++seen;
	CheckEq(seen, 1, "a second view of the index sees the registration");
	reader.Close();

	index.Remove(fakeA);
	index.Close();
	Check(!index.IsOpen(), "closing the index releases it");
}

void TestIntroSkip() {
	Section("the intro skip - which gGameState transitions it will force");

	// This is the safety argument from intro.h, turned into a test. The two
	// movie states are the only ones the skipper may act on. GS_INIT_ONCE (5)
	// and GS_INIT_FRONTEND (6) are where the one-time init lives, and
	// GS_INIT_LOGO_MPEG (1) comes before the logo movie's CoInitialize, so
	// jumping from it would leave the CoUninitialize in state 5 unbalanced.
	Check(ShouldSkipIntroFrom(2), "GS_LOGO_MPEG is skippable");
	Check(ShouldSkipIntroFrom(4), "GS_INTRO_MPEG is skippable");

	Check(!ShouldSkipIntroFrom(0), "GS_START_UP is not");
	Check(!ShouldSkipIntroFrom(1), "GS_INIT_LOGO_MPEG is not, no CoInitialize has happened yet");
	Check(!ShouldSkipIntroFrom(3), "GS_INIT_INTRO_MPEG is not");
	Check(!ShouldSkipIntroFrom(5), "GS_INIT_ONCE is not, InitialiseOnceAfterRW lives there");
	Check(!ShouldSkipIntroFrom(6), "GS_INIT_FRONTEND is not");
	Check(!ShouldSkipIntroFrom(7), "GS_FRONTEND is not");
	Check(!ShouldSkipIntroFrom(8), "GS_INIT_PLAYING_GAME is not");
	Check(!ShouldSkipIntroFrom(9), "GS_PLAYING_GAME is not");
	Check(!ShouldSkipIntroFrom(0xFFFFFFFFu), "and neither is a garbage read");

	// Walk every value it could ever see, so "acts on exactly two states" is
	// measured here rather than just asserted.
	int skippable = 0;
	for (uint32_t s = 0; s < 64; ++s)
		if (ShouldSkipIntroFrom(s))
			++skippable;
	CheckEq(skippable, 2, "exactly two states in the whole range are skippable");
}

void TestCdStreamName() {
	Section("the streaming semaphore rename - eight characters, one per process");

	char name[16];
	FormatCdStreamName(4312, name, sizeof(name));

	// Exactly as long as "CdStream", so the replacement fits the original
	// string's footprint and can't run into whatever follows it.
	CheckEq(std::strlen(name), 8, "the replacement is exactly as long as \"CdStream\"");
	Check(name[0] == 'C' && name[1] == 'd', "it still starts with Cd, so a hex dump makes sense");
	Check(std::strcmp(name, "CdStream") != 0, "and it is not the name it replaces");

	// Injective over pids - two live games must not land on the same name,
	// or the rename has achieved nothing.
	char a[16], b[16], c[16];
	FormatCdStreamName(0, a, sizeof(a));
	FormatCdStreamName(1, b, sizeof(b));
	FormatCdStreamName(0xFFFFFFFFu, c, sizeof(c));
	Check(std::strcmp(a, b) != 0, "adjacent pids differ");
	Check(std::strcmp(a, c) != 0, "and so do the extremes");
	CheckEq(std::strlen(a), 8, "pid 0 is still eight characters");
	CheckEq(std::strlen(c), 8, "and so is the widest pid, 62^6 covers all of them");

	// Every character has to be legal in a kernel object name. Base-62 is;
	// a backslash would silently put the object in a different directory.
	for (const char *p = c; *p; ++p)
		Check((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'),
		      "every character is alphanumeric");

	char tiny[4] = {'x', 'x', 'x', 'x'};
	FormatCdStreamName(1, tiny, sizeof(tiny));
	Check(tiny[0] == 'x', "a buffer too small is left alone rather than truncated");
}

void TestSettings() {
	Section("AgentPad.ini - both keys are conservative until somebody says otherwise");

	Settings def;
	Check(!def.skipIntro, "SkipIntro is OFF by default");
	Check(def.multiInstance == Settings::MultiInstance::Auto, "MultiInstance defaults to auto");

	Settings s;
	s.ParseIni("SkipIntro=1\nMultiInstance=on\n");
	Check(s.skipIntro, "SkipIntro=1 turns it on");
	Check(s.multiInstance == Settings::MultiInstance::On, "MultiInstance=on");

	s = Settings();
	s.ParseIni("; a comment\n[section]\n  skipintro = yes  \nmultiinstance=OFF\n");
	Check(s.skipIntro, "keys are case- and space-insensitive, comments and sections ignored");
	Check(s.multiInstance == Settings::MultiInstance::Off, "MultiInstance=off");

	s = Settings();
	s.ParseIni("SkipIntro=banana\nMultiInstance=banana\n");
	Check(!s.skipIntro, "an unparseable value leaves the default alone");
	Check(s.multiInstance == Settings::MultiInstance::Auto, "for both keys");

	s = Settings();
	s.ParseIni("UnknownKey=1\nnot an assignment\n");
	Check(!s.skipIntro, "unknown keys and junk lines are ignored, not fatal");

	s = Settings();
	s.ParseIni("");
	Check(!s.skipIntro, "empty text is not an error");

	// The environment wins over the file - the ini is the machine's setting,
	// the environment is this run's own.
	s = Settings();
	s.ParseIni("SkipIntro=0\nMultiInstance=off\n");
	s.ApplyOverrides("1", "on");
	Check(s.skipIntro, "AGENTPAD_SKIPINTRO overrides the file");
	Check(s.multiInstance == Settings::MultiInstance::On, "AGENTPAD_MULTIINSTANCE too");

	s.ApplyOverrides(nullptr, nullptr);
	Check(s.skipIntro, "an unset variable changes nothing");
	s.ApplyOverrides("", "");
	Check(s.skipIntro, "and neither does an empty one");

	Check(std::strcmp(Settings::MultiInstanceName(Settings::MultiInstance::Auto), "auto") == 0,
	      "the mode has a name for the log");
}

void TestRealSection() {
	Section("the real named section - created and opened in this process");

	// Our own pid, which is the whole point of per-instance naming. This test
	// used to create "CoopIII.AgentPad.v1" outright, and whenever GTA III
	// happened to be running it would attach to the live game's section
	// instead of a fresh one, write a pressed Cross into it, and then fail
	// its own last two checks because the section was neither empty nor
	// going away.
	const uint32_t pid = SelfPid();

	Channel mod;
	if (!mod.Create(pid)) {
		std::printf("  SKIP  cannot create the section here (%s)\n", mod.Error());
		return;
	}
	Check(mod.IsOpen(), "the mod can create the section");

	// A freshly created section must read as unclaimed.
	Decision none;
	CheckEq(Decide(*mod.Get(), 0, none).status, STATUS_BAD_HEADER,
	        "a freshly created section is unclaimed");

	// A second opener sees the same memory - that's the whole mechanism.
	Channel driver;
	Check(driver.Open(pid), "a driver can open the section by pid");
	Check(driver.Get() != nullptr, "the driver gets a view");

	driver.Get()->magic    = MAGIC;
	driver.Get()->version  = VERSION;
	driver.Get()->enabled  = 1;
	driver.Get()->channels = CHANNEL_PAD;
	driver.Get()->pad[IDX_CROSS] = BUTTON_DOWN;
	driver.Get()->seq      = 1;

	CheckEq(mod.Get()->pad[IDX_CROSS], BUTTON_DOWN, "a driver write is visible to the mod");
	CheckEq(mod.Get()->seq, 1, "the sequence number crosses the section");

	Decision d = Decide(*mod.Get(), 0, none);
	CheckEq(d.status, STATUS_INJECTING, "a claimed section injects");
	CheckEq(d.pad[IDX_CROSS], BUTTON_DOWN, "the pressed button reaches the decision");

	// And the mod's half is visible to the driver too - that's how a script
	// tells "not loaded" apart from "loaded and ignoring me".
	mod.Get()->modMagic  = MAGIC;
	mod.Get()->modFrames = 4242;
	CheckEq(driver.Get()->modMagic, (long long)MAGIC, "the driver can see the mod's magic");
	CheckEq(driver.Get()->modFrames, 4242, "the driver can see the mod's frame count");

	driver.Close();
	Check(!driver.IsOpen(), "closing a view releases it");

	// Create() on an existing section must attach, not wipe - a driver that
	// got there first has to survive the mod loading afterward.
	Channel second;
	Check(second.Create(pid), "Create attaches to an existing section");
	CheckEq(second.Get()->pad[IDX_CROSS], BUTTON_DOWN,
	        "Create does not wipe a section that already had a driver");

	second.Close();
	mod.Close();

	// With every handle gone the section is gone too, so a stale one can't
	// outlive the game and feed the next run someone else's input.
	Channel afterwards;
	Check(!afterwards.Open(pid), "the section disappears once nobody holds it");
	Check(!InstanceIsLive(pid), "and stops reporting as a live instance");
}

// Prints the layout the C++ side actually compiled to, as `name=value` lines,
// so tools/GtaInput/GtaInput.Tests.ps1 can diff it against what the
// PowerShell driver believes. Two hardcoded copies of a struct layout that
// only agree with themselves prove nothing - this is what makes them agree
// with each other, and turns a change to one into a failing test in the other.
void PrintContract() {
	std::printf("ShmPrefix=%s\n", SHM_PREFIX);
	std::printf("ShmIndexName=%s\n", SHM_INDEX_NAME);
	std::printf("ShmSize=%u\n", static_cast<unsigned>(sizeof(Shared)));
	std::printf("IndexSize=%u\n", static_cast<unsigned>(sizeof(Index)));
	std::printf("IndexSlots=%u\n", INDEX_SLOTS);
	std::printf("IndexOffset.Magic=%u\n", static_cast<unsigned>(offsetof(Index, magic)));
	std::printf("IndexOffset.Version=%u\n", static_cast<unsigned>(offsetof(Index, version)));
	std::printf("IndexOffset.Capacity=%u\n", static_cast<unsigned>(offsetof(Index, capacity)));
	std::printf("IndexOffset.Pid=%u\n", static_cast<unsigned>(offsetof(Index, pid)));
	std::printf("Magic=%u\n", MAGIC);
	std::printf("Version=%u\n", VERSION);

	std::printf("Offset.Magic=%u\n", static_cast<unsigned>(offsetof(Shared, magic)));
	std::printf("Offset.Version=%u\n", static_cast<unsigned>(offsetof(Shared, version)));
	std::printf("Offset.Seq=%u\n", static_cast<unsigned>(offsetof(Shared, seq)));
	std::printf("Offset.Enabled=%u\n", static_cast<unsigned>(offsetof(Shared, enabled)));
	std::printf("Offset.Channels=%u\n", static_cast<unsigned>(offsetof(Shared, channels)));
	std::printf("Offset.WriterTickMs=%u\n", static_cast<unsigned>(offsetof(Shared, writerTickMs)));
	std::printf("Offset.TimeoutMs=%u\n", static_cast<unsigned>(offsetof(Shared, timeoutMs)));
	std::printf("Offset.KeyMask=%u\n", static_cast<unsigned>(offsetof(Shared, keys)));
	std::printf("Offset.MouseBudgetX=%u\n", static_cast<unsigned>(offsetof(Shared, mouseBudgetX)));
	std::printf("Offset.MouseBudgetY=%u\n", static_cast<unsigned>(offsetof(Shared, mouseBudgetY)));
	std::printf("Offset.MouseSeq=%u\n", static_cast<unsigned>(offsetof(Shared, mouseSeq)));
	std::printf("Offset.MouseStepMax=%u\n", static_cast<unsigned>(offsetof(Shared, mouseStepMax)));
	std::printf("Offset.ModMagic=%u\n", static_cast<unsigned>(offsetof(Shared, modMagic)));
	std::printf("Offset.ModPid=%u\n", static_cast<unsigned>(offsetof(Shared, modPid)));
	std::printf("Offset.ModFrames=%u\n", static_cast<unsigned>(offsetof(Shared, modFrames)));
	std::printf("Offset.ModApplied=%u\n", static_cast<unsigned>(offsetof(Shared, modApplied)));
	std::printf("Offset.ModSeqSeen=%u\n", static_cast<unsigned>(offsetof(Shared, modSeqSeen)));
	std::printf("Offset.ModStatus=%u\n", static_cast<unsigned>(offsetof(Shared, modStatus)));
	std::printf("Offset.ModMouseLeftX=%u\n", static_cast<unsigned>(offsetof(Shared, modMouseLeftX)));
	std::printf("Offset.ModMouseLeftY=%u\n", static_cast<unsigned>(offsetof(Shared, modMouseLeftY)));
	std::printf("Offset.Pad=%u\n", static_cast<unsigned>(offsetof(Shared, pad)));
	std::printf("Offset.ModGameState=%u\n", static_cast<unsigned>(offsetof(Shared, modGameState)));

	std::printf("Channel.Pad=%u\n", CHANNEL_PAD);
	std::printf("Channel.Keys=%u\n", CHANNEL_KEYS);
	std::printf("Channel.Mouse=%u\n", CHANNEL_MOUSE);
	std::printf("ChannelAll=%u\n", CHANNEL_ALL);

	for (Key k : ALL_KEYS)
		std::printf("Key.%s=%u\n", KeyName(k), static_cast<unsigned>(k));

	for (int i = 0; i < IDX_COUNT; ++i)
		std::printf("Pad.%s=%d\n", PadFieldName(i), i);

	std::printf("ButtonDown=%d\n", BUTTON_DOWN);
	std::printf("AxisMax=%d\n", AXIS_MAX);
}

// Stands in for the game: creates the section, runs Engine::Step in a loop
// exactly as the hook does, and prints a line whenever the decision changes.
//
// This is what lets tools/GtaInput/GtaInput.E2E.Tests.ps1 drive the real
// decision engine from a real PowerShell driver, over a real named section,
// without GTA III. Covers the one thing nothing else can - that the bytes
// PowerShell writes are the bytes the mod reads. Only untested link left
// after this is the memcpy into CPad itself.
int Observe(unsigned timeoutMs) {
	const uint32_t pid = SelfPid();

	Channel mod;
	if (!mod.Create(pid)) {
		std::printf("ERROR %s\n", mod.Error());
		return 1;
	}

	// Register the same way the real mod does, so GtaInput.E2E.Tests.ps1
	// exercises Get-GtaInstance against a real index rather than a special
	// case.
	InstanceIndex index;
	const bool listed = index.Open(true) && index.Add(pid);

	Shared &shm  = *mod.Get();
	shm.modMagic = MAGIC;
	shm.modPid   = pid;

	// The pid goes on the READY line so a driver can target this observer
	// directly, without having to trust the index to find it.
	std::printf("READY pid=%u name=%s listed=%d\n", pid, mod.Name(), listed ? 1 : 0);
	std::fflush(stdout);

	Engine   engine;
	uint32_t lastSeq    = 0xFFFFFFFFu;
	uint32_t lastStatus = 0xFFFFFFFFu;
	unsigned elapsed    = 0;

	// 10ms isn't the game's frame time, but the engine never reads a clock of
	// its own (the watchdog runs off the tick we pass in), so the loop rate
	// only affects how finely we sample the driver.
	while (elapsed < timeoutMs) {
		const Engine::Frame f = engine.Step(shm, elapsed);

		if (f.status != lastStatus || shm.seq != lastSeq) {
			lastStatus = f.status;
			lastSeq    = shm.seq;

			std::printf("FRAME seq=%u status=%u channels=%u keys=%u mouse=%d,%d pad=",
			            shm.seq, f.status, f.channels, f.keys, f.mouseDx, f.mouseDy);
			for (int i = 0; i < IDX_COUNT; ++i)
				std::printf("%s%d", i ? "," : "", f.pad[i]);
			std::printf("\n");
			std::fflush(stdout);
		}

		Sleep(10);
		elapsed += 10;
	}

	index.Remove(pid);
	index.Close();

	std::printf("DONE frames=%u applied=%u\n", engine.Frames(), engine.Applied());
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc > 1 && std::strcmp(argv[1], "--contract") == 0) {
		PrintContract();
		return 0;
	}
	if (argc > 2 && std::strcmp(argv[1], "--observe") == 0)
		return Observe(static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)));

	std::printf("padtest: AgentPad shared-memory protocol and encoding\n");

	TestLayout();
	TestSectionNaming();
	TestHeaderValidation();
	TestEnableDisable();
	TestChannels();
	TestWatchdog();
	TestTornRead();
	TestMouseDrain();
	TestMouseBudgetPlumbing();
	TestEngine();
	TestKeyTable();
	TestPadTable();
	TestIntroSkip();
	TestCdStreamName();
	TestSettings();
	TestRealSection();
	TestInstanceIndex();

	std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
