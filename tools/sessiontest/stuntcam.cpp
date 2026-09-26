// A unique jump's shot, the server's half: server/core/stuntcam.h. Who may
// send it and who hears it, over players built by hand.

#include "stuntcam.h"

#include <cstdio>
#include <vector>

using namespace coopiii;

namespace {

int g_shotFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_shotFailures;
}

Player Seated(uint8_t id, uint16_t car, uint8_t seat) {
	Player p;
	p.id           = id;
	p.vehicleNetId = car;
	p.seat         = seat;
	return p;
}

StuntCameraBody Shot(uint16_t car, bool on) {
	StuntCameraBody b{};
	b.netId = car;
	b.on    = on ? 1 : 0;
	b.mode  = 15;
	b.swap  = 2;
	return b;
}

void TestWhoHearsAShot() {
	std::printf("\na unique jump's shot: who hears it\n");
	const std::vector<Player> players = {
	    Seated(0, 80, 0),              // alice drives 80
	    Seated(1, 80, 2),              // bob rides with her
	    Seated(2, 80, 3),              // carol too
	    Seated(3, 81, 1),              // dave rides in another car
	    Seated(4, INVALID_NETID, 0),   // erin is on foot beside them
	};

	const std::vector<uint8_t> to = StuntShotAudience(players, players[0], Shot(80, true));
	Check(to.size() == 2 && to[0] == 1 && to[1] == 2,
	      "the driver's shot goes to the two riding with her, and to nobody else");
	Check(StuntShotAudience(players, players[1], Shot(80, true)).empty(),
	      "a passenger's is refused: his game never runs the jump");
	Check(StuntShotAudience(players, players[4], Shot(80, true)).empty(),
	      "and so is one from somebody on foot");
	Check(StuntShotAudience(players, players[0], Shot(81, true)).empty(),
	      "a driver's shot of a car she is not in goes nowhere");
	Check(StuntShotAudience(players, players[0], Shot(INVALID_NETID, true)).empty() &&
	          StuntShotAudience(players, players[0], Shot(INVALID_NETID, false)).empty(),
	      "nor one of no car");

	// The jump failed because she got out; her exit reached the session
	// before the end did.
	const Player out = Seated(0, INVALID_NETID, 0);
	const std::vector<uint8_t> end = StuntShotAudience(players, out, Shot(80, false));
	Check(end.size() == 2 && end[0] == 1 && end[1] == 2,
	      "its end still reaches the riders from wherever she stands");
	Check(StuntShotSenderOk(players[0], Shot(80, true)) &&
	          !StuntShotSenderOk(players[1], Shot(80, true)) &&
	          StuntShotSenderOk(players[1], Shot(80, false)),
	      "only a start needs the wheel");

	const std::vector<Player> alone = {Seated(0, 80, 0)};
	Check(StuntShotAudience(alone, alone[0], Shot(80, true)).empty(),
	      "nobody riding, nobody told");
}

} // namespace

int RunStuntCameraTests() {
	g_shotFailures = 0;
	TestWhoHearsAShot();
	return g_shotFailures;
}
