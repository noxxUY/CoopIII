// A car's alarm and where its gun points (protocol.h, S_VehicleAlarm and
// S_VehicleAim): who the session takes them from, and what a joiner is told.

#include "session.h"

#include <cstdio>
#include <limits>

using namespace coopiii;

namespace {

int g_carExtrasFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_carExtrasFailures;
}

Player *Join(Session &s, uint32_t peer, const char *nick) {
	RejectReason reject = REJECT_NONE;
	return s.AddPlayer(peer, nick, 7, PROTOCOL_VERSION, reject);
}

Vehicle *Claim(Session &s, Player &driver, uint16_t modelId) {
	Vehicle *v = s.AddVehicle(modelId, 3, 4, Vec3{10.0f, 10.0f, 1.0f},
	                          Quat{0.0f, 0.0f, 0.0f, 1.0f});
	if (v)
		s.NoteEnterVehicle(driver, *v, /*seat=*/0);
	return v;
}

void TestTheAlarmIsTheDriversToSay() {
	std::printf("\na car's alarm: who says so\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;
	s.NoteEnterVehicle(*bob, *car, /*seat=*/1);

	uint16_t ms = 14000;
	Check(!s.NoteVehicleAlarm(bob->id, netId, ms, 1000),
	      "a passenger's engine is not the one sounding it");
	Check(car->alarmEndsAtMs == 0, "and nothing is kept");
	Check(s.NoteVehicleAlarm(alice->id, netId, ms, 1000) && ms == 14000,
	      "the driver's is");
	Check(car->alarmEndsAtMs >= 15000 && car->alarmEndsAtMs <= 15001,
	      "and the session knows when it stops");

	ms = 60000;
	Check(s.NoteVehicleAlarm(alice->id, netId, ms, 2000) && ms == VEHICLE_ALARM_MS,
	      "more than the engine ever sets is taken as the engine's 15 s");

	ms = 0;
	Check(s.NoteVehicleAlarm(alice->id, netId, ms, 3000), "a stop while it sounds is news");
	Check(car->alarmEndsAtMs == 0, "and ends it");
	ms = 0;
	Check(!s.NoteVehicleAlarm(alice->id, netId, ms, 3100),
	      "a stop for an alarm nobody heard is nothing to tell anybody");

	ms = 5000;
	Check(!s.NoteVehicleAlarm(alice->id, 999, ms, 3200), "an unknown car is refused");
}

void TestAJoinerHearsWhatIsLeft() {
	std::printf("\na car's alarm: a joiner\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Vehicle *car  = Claim(s, *alice, 91);
	const uint16_t netId = car->netId;

	uint16_t ms = 10000;
	s.NoteVehicleAlarm(alice->id, netId, ms, 1000);

	Player *bob = Join(s, 2, "bob");
	const Backfill back = s.BuildBackfill(bob->id, 7000);
	Check(back.alarms.size() == 1 && back.alarms[0].netId == netId,
	      "the alarm still going is in the backfill");
	Check(!back.alarms.empty() && back.alarms[0].remainingMs >= 3999 &&
	          back.alarms[0].remainingMs <= 4001,
	      "with what is left of it, not what it started with");
	Check(!back.alarms.empty() && back.alarms[0].playerId == INVALID_PLAYER,
	      "as the session's own record");

	const Backfill late = s.BuildBackfill(bob->id, 12000);
	Check(late.alarms.empty(), "one that has run out is not");

	Player *carol = Join(s, 3, "carol");
	Vehicle *quiet = Claim(s, *carol, 92);
	const Backfill none = s.BuildBackfill(bob->id, 2000);
	Check(quiet && none.alarms.size() == 1, "a car whose alarm never went off is never in it");
}

void TestTheTurretIsTheDriversToAim() {
	std::printf("\nwhere a car's gun points\n");
	Session s;
	Player *alice = Join(s, 1, "alice");
	Player *bob   = Join(s, 2, "bob");
	Vehicle *tank = Claim(s, *alice, 122);
	const uint16_t netId = tank->netId;

	Check(!tank->aimKnown, "nobody has aimed a new one");
	Check(!s.NoteVehicleAim(bob->id, netId, 1.0f, 0.0f), "somebody not driving it is refused");
	Check(s.NoteVehicleAim(alice->id, netId, 1.5f, 0.1f), "the driver is not");
	Check(tank->aimKnown && tank->gunLR == 1.5f && tank->gunUD == 0.1f, "and it is kept");

	const float qnan = std::numeric_limits<float>::quiet_NaN();
	Check(!s.NoteVehicleAim(alice->id, netId, qnan, 0.1f), "a NaN is refused");
	Check(!s.NoteVehicleAim(alice->id, netId, 1e9f, 0.1f), "and so is nonsense");
	Check(tank->gunLR == 1.5f, "and neither moves it");

	Player *carol = Join(s, 3, "carol");
	const Backfill back = s.BuildBackfill(carol->id, 1000);
	Check(back.aims.size() == 1 && back.aims[0].netId == netId &&
	          back.aims[0].gunLR == 1.5f && back.aims[0].gunUD == 0.1f &&
	          back.aims[0].playerId == INVALID_PLAYER,
	      "a joiner is told where it points");

	Vehicle *car = Claim(s, *bob, 91);
	const Backfill again = s.BuildBackfill(carol->id, 1000);
	Check(car && again.aims.size() == 1, "a car nobody has aimed is not in it");
}

} // namespace

int RunCarExtrasTests() {
	TestTheAlarmIsTheDriversToSay();
	TestAJoinerHearsWhatIsLeft();
	TestTheTurretIsTheDriversToAim();
	return g_carExtrasFailures;
}
