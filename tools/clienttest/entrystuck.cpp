// entrystuck.h: a local player who can no longer get into any car.
//
// Reported from an internet session: after getting out of a car with the
// seat key the enter key walked him up to every car and never got him in.
// The cause was the car animation the seat key left on him; these hold the
// watch that notices that state and the session record that outlives it.

#include "client.h"
#include "entrystuck.h"

#include <cstdio>

#include <cstring>

using namespace coopiii;

namespace {

int g_stuckFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_stuckFailures;
}

template <class T>
Message Wrap(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

LocalEntryProbe OnFootAt(float x, float y) {
	LocalEntryProbe p;
	p.havePed        = true;
	p.inControlState = true;
	p.controlsOn     = true;
	p.x              = x;
	p.y              = y;
	return p;
}

void TestTheStaleCarAnimationIsFound() {
	std::printf("\nthe car animation left on a player on foot\n");
	EntryStuckWatch w;
	LocalEntryProbe p = OnFootAt(0, 0);
	p.vehicleAnim     = true;
	Check(w.Tick(p, false, 1000) == EntryStuck::None, "not on the first look");
	Check(w.Tick(p, false, 1100) == EntryStuck::None, "nor a moment later");
	Check(w.Tick(p, false, 1000 + ENTRY_STALE_ANIM_MS) == EntryStuck::StaleVehicleAnim,
	      "but once it has outlasted a frame, it is taken off");
	Check(w.Tick(p, false, 1000 + ENTRY_STALE_ANIM_MS + 40) == EntryStuck::None,
	      "and it is not asked for again on the next tick");

	EntryStuckWatch seated;
	LocalEntryProbe in = OnFootAt(0, 0);
	in.inVehicle       = true;
	in.seatedState     = true;
	in.inControlState  = false;
	in.vehicleAnim     = true;
	for (uint32_t t = 0; t <= 10000; t += 40)
		seated.Tick(in, false, t);
	Check(seated.Tick(in, false, 10040) == EntryStuck::None, "a seated player keeps his");

	EntryStuckWatch opening;
	LocalEntryProbe door = OnFootAt(0, 0);
	door.entering        = true;
	door.inControlState  = false;
	door.vehicleAnim     = true;
	EntryStuck any       = EntryStuck::None;
	for (uint32_t t = 0; t <= 2000; t += 40) {
		door.animMark = t;
		if (opening.Tick(door, false, t) != EntryStuck::None)
			any = EntryStuck::NoProgress;
	}
	Check(any == EntryStuck::None, "and so does one opening a door");
}

void TestSittingInNoCar() {
	std::printf("\nsitting in no car at all\n");
	EntryStuckWatch w;
	LocalEntryProbe p = OnFootAt(0, 0);
	p.seatedState     = true;
	p.inControlState  = false;
	bool early = false;
	for (uint32_t t = 0; t < ENTRY_SEATED_ONFOOT_MS; t += 40)
		early = early || w.Tick(p, false, t) != EntryStuck::None;
	Check(!early, "given a moment");
	Check(w.Tick(p, false, ENTRY_SEATED_ONFOOT_MS) == EntryStuck::SeatedOnFoot,
	      "then stood up");
}

void TestAnEntryGoingNowhere() {
	std::printf("\nwalking to a door that never opens\n");
	EntryStuckWatch w;
	LocalEntryProbe p = OnFootAt(0, 0);
	p.enterObjective  = true;
	p.seeking         = true;
	EntryStuck got    = EntryStuck::None;
	uint32_t   at     = 0;
	// Walking up to it: ten metres in two seconds.
	for (uint32_t t = 0; t <= 2000; t += 40) {
		p.x = t / 200.0f;
		if (w.Tick(p, false, t) != EntryStuck::None)
			got = EntryStuck::NoProgress;
	}
	Check(got == EntryStuck::None, "a player walking to the car is getting somewhere");
	// Then standing at the door.
	for (uint32_t t = 2040; t <= 2000 + ENTRY_STUCK_MS + 80 && got == EntryStuck::None; t += 40) {
		got = w.Tick(p, false, t);
		at  = t;
	}
	Check(got == EntryStuck::NoProgress, "standing at the door is called off");
	Check(at + 500 >= 2000 + ENTRY_STUCK_MS, "and not before ENTRY_STUCK_MS");

	EntryStuckWatch scripted;
	LocalEntryProbe s = p;
	s.controlsOn      = false;
	got               = EntryStuck::None;
	for (uint32_t t = 0; t <= 20000; t += 40)
		if (scripted.Tick(s, false, t) != EntryStuck::None)
			got = EntryStuck::NoProgress;
	Check(got == EntryStuck::None,
	      "a script walking him there with the controls off is left alone");

	EntryStuckWatch own;
	got = EntryStuck::None;
	for (uint32_t t = 0; t <= 20000; t += 40)
		if (own.Tick(p, true, t) != EntryStuck::None)
			got = EntryStuck::NoProgress;
	Check(got == EntryStuck::None, "and so is the seat key's own entry");

	EntryStuckWatch paused;
	got = paused.Tick(p, false, 0);
	got = paused.Tick(p, false, 60000);
	Check(got == EntryStuck::None, "a minute in the menu is not a minute at the door");
}

void TestTheSessionsSeatIsHeldToTheEngine() {
	std::printf("\na seat the session holds for a player on foot\n");
	SessionSeatWatch w;
	Check(!w.Tick(80, false, false, 0), "seated: nothing");
	bool early = false;
	for (uint32_t t = 40; t < SESSION_SEAT_STALE_MS; t += 40)
		early = early || w.Tick(80, true, false, t);
	Check(!early, "on foot: not before SESSION_SEAT_STALE_MS");
	Check(w.Tick(80, true, false, SESSION_SEAT_STALE_MS), "then let go");
	bool again = false;
	for (uint32_t t = 80 + SESSION_SEAT_STALE_MS; t < 80 + 3 * SESSION_SEAT_STALE_MS; t += 40)
		again = again || w.Tick(80, true, false, t);
	Check(!again, "once");
	Check(!w.Tick(INVALID_NETID, true, false, 300 + SESSION_SEAT_STALE_MS * 3), "no seat, nothing");

	// Through the client: the echo of our own claim writes the record, and a
	// player on foot for long enough sends the exit.
	Client c;
	WorldBridge b;
	static LocalEntryProbe s_probe;
	static uint32_t        s_resets = 0;
	static uint8_t         s_last   = 0;
	s_probe  = OnFootAt(0, 0);
	s_resets = 0;
	b.ProbeLocalEntry = [](LocalEntryProbe &out) {
		out = s_probe;
		return true;
	};
	b.ResetLocalEntry = [](uint8_t what) {
		++s_resets;
		s_last = what;
	};
	c.SetBridge(b);
	S_Welcome welcome{};
	InitHeader(welcome, 0);
	welcome.playerId     = 0;
	welcome.netId        = 100;
	welcome.maxPlayers   = MAX_PLAYERS;
	welcome.snapshotHz   = SNAPSHOT_HZ;
	welcome.hour         = 12;
	welcome.hostPlayerId = INVALID_PLAYER;
	c.HandleMessage(Wrap(welcome));
	S_EnterVehicle echo{};
	InitHeader(echo, 0);
	echo.playerId   = c.LocalPlayerId();
	echo.body.netId = 80;
	echo.body.seat  = 1;
	c.HandleMessage(Wrap(echo));
	Check(c.SessionSeatForTest() == 80, "the session's word is written down");

	for (uint32_t t = 1000; t < 1000 + SESSION_SEAT_STALE_MS; t += 40)
		c.TickEntryWatchdogForTest(t);
	Check(c.SessionSeatReleasesForTest() == 0, "not before SESSION_SEAT_STALE_MS");
	c.TickEntryWatchdogForTest(1000 + SESSION_SEAT_STALE_MS);
	Check(c.SessionSeatReleasesForTest() == 1, "then the exit goes out");
	Check(c.SessionSeatForTest() == INVALID_NETID, "and the record is let go of here too");

	// The stale animation, found and handed to the engine side.
	s_probe.vehicleAnim = true;
	for (uint32_t t = 10000; t <= 10000 + ENTRY_STALE_ANIM_MS + 80; t += 40)
		c.TickEntryWatchdogForTest(t);
	Check(s_resets == 1 && s_last == static_cast<uint8_t>(EntryStuck::StaleVehicleAnim),
	      "the car animation on a player on foot is asked off");
	Check(c.EntryResetsForTest() == 1, "once");

	// The seat key's walk is its own.
	s_probe.vehicleAnim = false;
	s_probe.enterObjective = true;
	s_probe.seeking        = true;
	c.StartOwnSeatEntryForTest(80);
	for (uint32_t t = 20000; t <= 20000 + 2 * ENTRY_STUCK_MS; t += 40)
		c.TickEntryWatchdogForTest(t);
	Check(c.EntryResetsForTest() == 1, "the seat key's own entry is left to its deadline");
}

} // namespace

int RunEntryStuckTests() {
	TestTheStaleCarAnimationIsFound();
	TestSittingInNoCar();
	TestAnEntryGoingNowhere();
	TestTheSessionsSeatIsHeldToTheEngine();
	return g_stuckFailures;
}
