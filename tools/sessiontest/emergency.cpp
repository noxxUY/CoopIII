// Medics and fire trucks, as the session decides them: Session::NotePedRevive
// and Session::MaySprayCannon (docs/protocol.md 1.36).

#include "session.h"

#include <cstdio>
#include <cstring>

using namespace coopiii;

namespace {

int g_emFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_emFailures;
}

Player *JoinAs(Session &s, uint32_t peer, const char *nick) {
	RejectReason reject = REJECT_NONE;
	return s.AddPlayer(peer, nick, 7, PROTOCOL_VERSION, reject);
}

AmbientPedBody Walker(float x, uint8_t flags = 0) {
	AmbientPedBody b{};
	b.modelId = 7;
	b.pedType = 4;
	b.flags   = flags;
	b.pos     = {x, 60.0f, 2.0f};
	return b;
}

AmbientCarBody Truck(float x) {
	AmbientCarBody b{};
	b.modelId = 97;
	b.pos     = {x, 50.0f, 2.0f};
	b.rot     = {0.0f, 0.0f, 0.0f, 1.0f};
	return b;
}

void TestAMedicsRevive() {
	std::printf("\na medic stands a dead pedestrian up, whoever's either of them is\n");
	Session s;
	Player *alice = JoinAs(s, 1, "alice");
	Player *bob   = JoinAs(s, 2, "bob");
	const uint16_t ped     = s.AddPed(alice->id, Walker(5.0f))->netId;
	const uint16_t enemy   = s.AddPed(alice->id, Walker(6.0f, AMBIENT_MISSION))->netId;

	Check(!s.NotePedRevive(ped), "a pedestrian who is alive cannot be revived");
	Check(s.NotePedDeath(PedDeathBody{ped, 13}, alice->id), "(alice's engine kills him)");
	Check(!s.FindPed(ped)->alive, "(and the session has him dead)");

	Check(s.NotePedRevive(ped), "bob's medic stands him up, though he is alice's pedestrian");
	Check(s.FindPed(ped)->alive && s.FindPed(ped)->deathAnimId == ANIM_NONE,
	      "and the session has him alive, with no death animation kept");
	Check(!s.NotePedRevive(ped), "a second medic's revive of the same life is refused");

	const Backfill back = s.BuildBackfill(bob->id, 1);
	bool corpse = false;
	for (const S_PedDeath &d : back.pedDeaths)
		corpse = corpse || d.body.netId == ped;
	Check(!corpse, "a joiner is handed him on his feet");

	Check(s.NotePedDeath(PedDeathBody{ped, 17}, alice->id),
	      "his next death is taken like his first");
	Check(s.NotePedRevive(ped), "and alice's own medic may stand him up again");

	Check(s.NotePedDeath(PedDeathBody{enemy, 13}, alice->id), "(the mission's man dies)");
	Check(!s.NotePedRevive(enemy), "no medic revives one of the mission's");
	Check(!s.NotePedRevive(999), "nor a pedestrian the session has never had");
}

void TestWhoAimsTheHose() {
	std::printf("\nwhose word a fire truck's jet is\n");
	Session s;
	Player *alice = JoinAs(s, 1, "alice");
	Player *bob   = JoinAs(s, 2, "bob");

	const uint16_t traffic = s.AddCar(alice->id, Truck(10.0f))->netId;
	Check(s.MaySprayCannon(alice->id, traffic), "the host of a traffic truck aims it");
	Check(!s.MaySprayCannon(bob->id, traffic), "nobody else does");
	UnownedVehicleKey key{};
	key.kind = UNOWNED_AMBIENT;
	key.id   = traffic;
	Check(s.NoteUnownedBlowUp(key, alice->id, 0), "(it blows up)");
	Check(!s.MaySprayCannon(alice->id, traffic), "and a wreck sprays nothing");

	Vehicle *car = s.AddVehicle(97, 3, 1, Vec3{10.0f, 10.0f, 1.0f}, Quat{0.0f, 0.0f, 0.0f, 1.0f});
	if (car)
		s.NoteEnterVehicle(*bob, *car, /*seat=*/0);
	Check(car != nullptr, "(bob drives a truck of the session's)");
	if (!car)
		return;
	Check(s.MaySprayCannon(bob->id, car->netId), "its driver aims it");
	Check(!s.MaySprayCannon(alice->id, car->netId), "and nobody beside him does");
	Check(!s.MaySprayCannon(bob->id, 999), "nor anybody a truck there is none of");
}

} // namespace

int RunEmergencyTests() {
	g_emFailures = 0;
	TestAMedicsRevive();
	TestWhoAimsTheHose();
	return g_emFailures;
}
