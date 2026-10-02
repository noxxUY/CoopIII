// Who works each crane, the server's half: server/core/cranes.h. The first
// busy state takes a crane, everybody else's is dropped until its worker ends
// it, goes quiet or leaves.

#include "cranes.h"

#include <cstdio>

using namespace coopiii;

namespace {

int g_craneFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_craneFailures;
}

CraneStateBody State(float x, float y, bool active, uint8_t state = 1) {
	CraneStateBody b{};
	b.craneX = x;
	b.craneY = y;
	b.active = active ? 1 : 0;
	b.state  = active ? state : 0;
	return b;
}

void TestTheFirstTakesTheCrane() {
	std::printf("\nthe cranes: who works one\n");
	CraneWorkers w;
	Check(w.Accept(1, State(1119.0f, 48.0f, true), 1000), "the first busy state takes it");
	Check(w.WorkerAt(1119.2f, 47.9f) == 1, "and names the crane by where it stands");
	Check(!w.Accept(2, State(1119.0f, 48.0f, true), 1010),
	      "somebody else's crane busy in the same round trip is dropped");
	Check(w.Accept(1, State(1119.0f, 48.0f, true), 1040), "the worker's next state goes on");
	Check(w.Accept(2, State(-400.0f, 200.0f, true), 1050), "another crane is another worker");
	Check(w.Count() == 2, "two cranes worked");

	Check(!w.Accept(2, State(1119.0f, 48.0f, false), 1060),
	      "an end from somebody who was not working it goes nowhere");
	Check(w.Accept(1, State(1119.0f, 48.0f, false), 1070), "the worker's end goes on");
	Check(w.WorkerAt(1119.0f, 48.0f) == INVALID_PLAYER, "and the crane is nobody's");
	Check(w.Accept(2, State(1119.0f, 48.0f, true), 1080), "so the next busy state takes it");
}

void TestAQuietWorkerLosesIt() {
	std::printf("\nthe cranes: a worker gone quiet\n");
	CraneWorkers w;
	w.Accept(1, State(1119.0f, 48.0f, true), 1000);
	Check(!w.Accept(2, State(1119.0f, 48.0f, true), 1000 + CRANE_FOLLOW_TIMEOUT_MS - 1),
	      "not before the timeout");
	Check(w.Accept(2, State(1119.0f, 48.0f, true), 1000 + CRANE_FOLLOW_TIMEOUT_MS),
	      "after it, the next busy state takes it");
	Check(w.WorkerAt(1119.0f, 48.0f) == 2, "and he works it now");
	Check(!w.Accept(1, State(1119.0f, 48.0f, true), 1000 + CRANE_FOLLOW_TIMEOUT_MS + 10),
	      "the one who went quiet does not get it back by speaking up");
}

void TestALeaverEndsHisCranes() {
	std::printf("\nthe cranes: a worker who leaves\n");
	CraneWorkers w;
	w.Accept(1, State(1119.0f, 48.0f, true), 1000);
	w.Accept(1, State(-400.0f, 200.0f, true), 1000);
	w.Accept(2, State(900.0f, -300.0f, true), 1000);
	const std::vector<CraneStateBody> ended = w.Forget(1);
	Check(ended.size() == 2 && ended[0].active == 0 && ended[1].active == 0,
	      "both of his cranes end, to be said to everybody");
	Check(SameCrane(ended[0].craneX, ended[0].craneY, 1119.0f, 48.0f) &&
	          CraneStateSane(ended[0]),
	      "each named by where it stands, and a state anybody may read");
	Check(w.Count() == 1 && w.WorkerAt(900.0f, -300.0f) == 2, "the other worker keeps his");
}

void TestNonsenseIsRefused() {
	std::printf("\nthe cranes: what is refused\n");
	CraneWorkers w;
	CraneStateBody b = State(1119.0f, 48.0f, true, 7);
	Check(!w.Accept(1, b, 1000), "a state no crane has");
	b       = State(1119.0f, 48.0f, true);
	b.hookX = 1e9f;
	Check(!w.Accept(1, b, 1000), "a hook off the map");
	Check(!w.Accept(INVALID_PLAYER, State(1119.0f, 48.0f, true), 1000), "nobody");
	for (int i = 0; i < 20; ++i)
		w.Accept(1, State(100.0f * static_cast<float>(i), 0.0f, true), 1000);
	Check(w.Count() == CraneWorkers::MAX_WORKED, "never more cranes than CCranes has");
}

} // namespace

int RunCraneWorkerTests() {
	TestTheFirstTakesTheCrane();
	TestAQuietWorkerLosesIt();
	TestALeaverEndsHisCranes();
	TestNonsenseIsRefused();
	return g_craneFailures;
}
