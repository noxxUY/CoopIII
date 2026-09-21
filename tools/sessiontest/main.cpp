// Unit tests for server/src/session.cpp, with no socket and no game.
//
//   xmake build sessiontest && xmake run sessiontest
//
// Session had no coverage of its own while it was only a roster and a
// counter: nettest drives two real clients through it and that was enough.
// It now decides which player the whole session takes its time of day from,
// and that decision is the kind that looks right and is wrong in a case
// nobody reaches by hand - a slot getting reused, a clock a minute short of
// a rollover.

#include "session.h"

#include <cstdio>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

Player *Join(Session &s, uint32_t peer, const char *nick) {
	RejectReason reject = REJECT_NONE;
	return s.AddPlayer(peer, nick, 7, PROTOCOL_VERSION, reject);
}

void TestHostIsTheFirstPlayerIn() {
	std::printf("\npicking a host\n");
	Session s;
	Check(s.HostId() == INVALID_PLAYER, "an empty session has no host");

	const Player *alice = Join(s, 1, "alice");
	Check(alice != nullptr && s.HostId() == alice->id, "the first player in gets it");

	const Player *bob = Join(s, 2, "bob");
	Check(bob != nullptr && s.HostId() == alice->id, "and keeps it when someone joins");
}

void TestAJoinerNeverTakesTheHostFromSomeoneStillHere() {
	std::printf("\na reused slot\n");
	Session s;
	Join(s, 1, "alice");   // slot 0
	Join(s, 2, "bob");     // slot 1
	Check(s.HostId() == 0, "alice has it");

	s.RemovePeer(1);
	Check(s.HostId() == 1, "alice leaves, bob takes it");

	// Slot 0 is free again, so the next player in fills it. Handing them the
	// session's clock because their slot number is lower would drag everyone
	// else to whatever time a machine that just connected is showing.
	const Player *carol = Join(s, 3, "carol");
	Check(carol != nullptr && carol->id == 0, "carol lands in the slot alice left");
	Check(s.HostId() == 1, "and bob still has it");
}

void TestTheLastPlayerOutTakesItWithThem() {
	std::printf("\nthe session emptying\n");
	Session s;
	Join(s, 1, "alice");
	Join(s, 2, "bob");

	s.RemovePeer(2);
	Check(s.HostId() == 0, "a non-host leaving changes nothing");

	s.RemovePeer(1);
	Check(s.HostId() == INVALID_PLAYER, "and the last one out leaves nobody holding it");
}

void TestTheClockAcceptsOnlyRealTimes() {
	std::printf("\nsetting the session clock\n");
	GameClock c;
	Check(c.Hour() == 12 && c.Minute() == 0, "it starts at noon, the way CClock does");

	Check(c.Set(3, 30) && c.Hour() == 3 && c.Minute() == 30, "a real time is taken");
	Check(!c.Set(24, 0) && c.Hour() == 3, "hour 24 is refused and changes nothing");
	Check(!c.Set(12, 60) && c.Minute() == 30, "and so is minute 60");
	Check(c.Set(23, 59), "the last minute of the day is a real time");

	// Set clears the part-minute. Without that, a report landing just short
	// of a rollover would tick the minute straight back off again.
	c.Set(12, 0);
	c.Advance(GameClock::MS_PER_GAME_MINUTE - 1);
	Check(c.Minute() == 0, "a fresh minute starts from the report, not from before it");
	c.Advance(2);
	Check(c.Minute() == 1, "and then runs on at the game's own rate");
}

void TestTheClockRollsOver() {
	std::printf("\nmidnight\n");
	GameClock c;
	c.Set(23, 59);
	c.Advance(GameClock::MS_PER_GAME_MINUTE);
	Check(c.Hour() == 0 && c.Minute() == 0, "23:59 plus a minute is midnight");

	c.Set(12, 0);
	c.Advance(GameClock::MS_PER_GAME_MINUTE * 60 * 24);
	Check(c.Hour() == 12 && c.Minute() == 0, "a whole day comes back to where it started");
}

void TestWeatherIsAPairAndIsChecked() {
	std::printf("\nthe session's sky\n");
	Session s;
	Check(s.Weather() == 0 && s.WeatherOld() == 0, "sunny either side to begin with");

	Check(s.SetWeather(2, 1), "rainy out of cloudy is a real pair");
	Check(s.Weather() == 2 && s.WeatherOld() == 1, "and both ends are kept");

	// eWeatherType is 0..3 and CWeather indexes arrays with it without a
	// bounds check, so this would be a crash on every client rather than a
	// wrong sky on one.
	Check(!s.SetWeather(4, 0), "there is no weather type 4");
	Check(!s.SetWeather(0, 200), "and none at 200 either");
	Check(s.Weather() == 2 && s.WeatherOld() == 1, "a refused pair changes nothing");
}

} // namespace

int main() {
	TestHostIsTheFirstPlayerIn();
	TestAJoinerNeverTakesTheHostFromSomeoneStillHere();
	TestTheLastPlayerOutTakesItWithThem();
	TestTheClockAcceptsOnlyRealTimes();
	TestTheClockRollsOver();
	TestWeatherIsAPairAndIsChecked();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
