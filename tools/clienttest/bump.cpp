// Ramming a car another machine simulates: client/src/bumpsync.h and
// client/src/game/bump.h.
//
// The arithmetic both ends run, and - with a retail exe handed over - the slot
// and the order of the frame the speed notes rest on. The roster's half, a
// bump going out and one coming in, is in main.cpp.

#include "bumpsync.h"
#include "game/addresses.h"
#include "game/bump.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_bumpFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_bumpFailures;
}

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

CopyContact Hit(float dx, float impulse, uint8_t piece = 2) {
	CopyContact c;
	c.fresh      = true;
	c.impulse    = impulse;
	c.piece      = piece;
	c.byOurWheel = true;
	c.move       = Vec3{dx, 0.0f, 0.0f};
	c.turn       = Vec3{0.0f, 0.0f, dx / 10.0f};
	return c;
}

void TestTheWire() {
	std::printf("\na bump on the wire\n");
	Check(PackBumpComponent(0.25f) == 2048 && Near(UnpackBumpComponent(2048), 0.25f),
	      "a quarter of a step's speed goes through");
	Check(PackBumpComponent(100.0f) == 32767 && PackBumpComponent(-100.0f) == -32767,
	      "a wild one is held to the field");
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(PackBumpComponent(nan) == 0 && PackBumpImpulse(nan) == 0 && PackBumpImpulse(-3.0f) == 0,
	      "nothing that is not a number");
	Check(PackBumpImpulse(312.6f) == 313 && PackBumpImpulse(1.0e9f) == 65535, "the impulse, whole");

	VehicleBumpBody body{};
	body.move[0] = 32767;
	body.move[1] = 32767;
	body.turn[2] = -32767;
	body.impulse = 400;
	Vec3  move, turn;
	float impulse = 0.0f;
	BumpFromWire(body, move, turn, impulse);
	Check(Near(std::sqrt(move.x * move.x + move.y * move.y), BUMP_MOVE_MAX) && Near(move.x, move.y),
	      "a received change of speed is held to BUMP_MOVE_MAX, keeping its direction");
	Check(Near(turn.z, -BUMP_TURN_MAX) && impulse == 400.0f, "and the spin to BUMP_TURN_MAX");
}

void TestTheRammersSide() {
	std::printf("\nour car into a copy\n");
	BumpOut b;
	NoteBump(b, Hit(0.1f, 120.0f, 3), true, 1000);
	Check(b.loose && b.pending, "the copy is ours for a moment and the hit waits to go out");
	VehicleBumpBody out{};
	Check(TakeBump(b, 1000, out) && out.move[0] == PackBumpComponent(0.1f) && out.impulse == 120 &&
	          out.piece == 3,
	      "it goes out at once, the speed and the impulse with its piece");
	NoteBump(b, Hit(0.02f, 40.0f, 5), true, 1030);
	NoteBump(b, Hit(0.03f, 60.0f, 6), true, 1060);
	Check(!TakeBump(b, 1060, out), "the next waits VEHICLE_BUMP_EVERY_MS");
	Check(TakeBump(b, 1000 + VEHICLE_BUMP_EVERY_MS, out) &&
	          out.move[0] == PackBumpComponent(0.05f) && out.impulse == 60 && out.piece == 6,
	      "and carries both frames since, the biggest impulse's piece");
	Check(!TakeBump(b, 2000, out), "nothing is sent with nothing new");

	BumpOut parked;
	NoteBump(parked, Hit(0.1f, 120.0f), false, 1000);
	Check(parked.loose && !parked.pending,
	      "a car nobody holds is only kept loose: the shove settles it");

	BumpOut w;
	NoteBump(w, Hit(0.1f, 50.0f), true, 1000);
	Check(StepBump(w, 1000 + BUMP_LOOSE_MS - 1) == BumpPhase::Loose, "loose for BUMP_LOOSE_MS");
	NoteBump(w, Hit(0.1f, 50.0f), true, 1500);
	Check(StepBump(w, 1500 + BUMP_LOOSE_MS - 1) == BumpPhase::Loose,
	      "and from the last contact while the shoving goes on");
	for (uint32_t t = 1500; t < 1000 + 2 * BUMP_LOOSE_MAX_MS; t += 100)
		NoteBump(w, Hit(0.01f, 10.0f), true, t);
	Check(StepBump(w, 1000 + BUMP_LOOSE_MAX_MS) == BumpPhase::StartBlend,
	      "but never past BUMP_LOOSE_MAX_MS");

	VehicleTransform ours, stream;
	ours.pos   = Vec3{12.0f, 0.0f, 0.0f};
	stream.pos = Vec3{10.0f, 0.0f, 0.0f};
	ours.rot   = Quat{0.0f, 0.0f, 0.7071068f, 0.7071068f};
	Check(BeginBlendBack(w, ours, stream, 5000), "the window closes two metres off the stream");
	Check(StepBump(w, 5000) == BumpPhase::Blend, "and blends back");
	VehicleTransform p = BlendedPose(w, stream, 5000);
	Check(Near(p.pos.x, 12.0f) && Near(p.rot.z, ours.rot.z), "from where our engine left it");
	stream.pos.x = 11.0f;
	p            = BlendedPose(w, stream, 5000 + BUMP_BLEND_MS / 2);
	Check(Near(p.pos.x, 12.0f), "the offset halved onto a stream that has moved on");
	p = BlendedPose(w, stream, 5000 + BUMP_BLEND_MS);
	Check(Near(p.pos.x, 11.0f) && Near(p.rot.w, 1.0f), "onto the stream");
	Check(StepBump(w, 5000 + BUMP_BLEND_MS) == BumpPhase::Stream, "and then it is the stream's");

	BumpOut far;
	ours.pos.x = 10.0f + VehicleInterpBuffer::SNAP_DISTANCE + 1.0f;
	stream.pos.x = 10.0f;
	Check(!BeginBlendBack(far, ours, stream, 5000) && StepBump(far, 5000) == BumpPhase::Stream,
	      "a teleport apart is snapped, as always");
}

void TestTheOwnersSide() {
	std::printf("\na bump on a car of ours\n");
	BumpInbox in;
	VehicleBumpBody body{};
	body.netId   = 81;   // ours
	body.byNetId = 80;   // theirs, which rammed it
	Check(in.Add(1, body, 1000) && in.PendingCount() == 1, "held");
	PendingBump &p = in.Pending()[0];
	Check(in.Judge(p, 1000 + BUMP_HOLD_MS - 1) == BumpVerdict::Wait, "for BUMP_HOLD_MS");
	Check(in.Judge(p, 1000 + BUMP_HOLD_MS) == BumpVerdict::Apply, "and then put on the car");
	in.NoteTouch(81, 80, 1100);
	Check(in.Judge(p, 1000 + BUMP_HOLD_MS) == BumpVerdict::Drop,
	      "unless our engine had the two touching meanwhile: that was this collision");
	BumpInbox before;
	before.NoteTouch(81, 80, 1000 - BUMP_HOLD_MS);
	before.Add(1, body, 1000);
	Check(before.Judge(before.Pending()[0], 1300) == BumpVerdict::Drop, "or just before");
	BumpInbox longAgo;
	longAgo.NoteTouch(81, 80, 1000 - BUMP_HOLD_MS - 1);
	longAgo.NoteTouch(81, 99, 1000);
	longAgo.NoteTouch(77, 80, 1000);
	longAgo.Add(1, body, 1000);
	Check(longAgo.Judge(longAgo.Pending()[0], 1300) == BumpVerdict::Apply,
	      "not a touch long before, nor another pair");
	Check(longAgo.Judge(longAgo.Pending()[0], 1000 + BUMP_STALE_MS + 1) == BumpVerdict::Drop,
	      "and a bump left far too long is not this collision any more");

	BumpInbox full;
	for (size_t i = 0; i < MAX_PENDING_BUMPS; ++i)
		full.Add(1, body, 1000);
	Check(!full.Add(1, body, 1000), "a full inbox says so");
	BumpInbox many;
	for (uint16_t i = 0; i < 40; ++i)
		many.NoteTouch(81, uint16_t(200 + i), 1000 + i);
	Check(many.TouchedSince(81, 239, 1000) && !many.TouchedSince(81, 200, 1000),
	      "the touches keep the newest");
}

void TestTheSpeedNotes() {
	std::printf("\neach car's speed between its ProcessControl and the collisions\n");
	SpeedSamples s;
	int a = 0, b = 0;
	Vec3 m, t;
	Check(s.Note(&a, 7, Vec3{1.0f, 0.0f, 0.0f}, Vec3{}) && s.Note(&b, 7, Vec3{2.0f, 0.0f, 0.0f}, Vec3{}),
	      "noted");
	Check(s.Take(&b, 7, m, t) && m.x == 2.0f && s.Take(&a, 7, m, t) && m.x == 1.0f,
	      "each car its own");
	Check(!s.Take(&a, 7, m, t), "once: a paused frame does not read the same collision again");
	Check(!s.Take(&b, 8, m, t), "and only the frame it was noted in");
	s.Note(&a, 8, Vec3{3.0f, 0.0f, 0.0f}, Vec3{});
	s.Note(&a, 8, Vec3{4.0f, 0.0f, 0.0f}, Vec3{});
	Check(s.Take(&a, 8, m, t) && m.x == 4.0f, "a postponed ProcessControl's second run is the one");
	std::vector<int> cars(SpeedSamples::SIZE + 4);
	size_t noted = 0;
	for (int &c : cars)
		noted += s.Note(&c, 9, Vec3{}, Vec3{}) ? 1 : 0;
	Check(noted == SpeedSamples::SIZE && s.Take(&cars[0], 9, m, t),
	      "a full frame keeps the table's worth, and finds them");
	const Vec3 d = SpeedChange(Vec3{0.3f, 0.1f, 0.0f}, Vec3{0.1f, 0.1f, 0.0f});
	Check(Near(d.x, 0.2f) && d.y == 0.0f, "what the collisions did is the difference");
}

// ---- against the real exe ---------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image, std::string &from) {
	std::vector<std::string> candidates;
	if (const char *env = std::getenv("COOPIII_GTA3_EXE"))
		candidates.push_back(env);
	candidates.push_back("reference/bin/gta3.exe");
	candidates.push_back("../../../../reference/bin/gta3.exe");
	for (const std::string &path : candidates) {
		FILE *fh = std::fopen(path.c_str(), "rb");
		if (!fh)
			continue;
		std::fseek(fh, 0, SEEK_END);
		const long size = std::ftell(fh);
		std::fseek(fh, 0, SEEK_SET);
		image.resize(size > 0 ? size_t(size) : 0);
		const size_t got = image.empty() ? 0 : std::fread(image.data(), 1, image.size(), fh);
		std::fclose(fh);
		if (got == image.size() && image.size() == IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool CallsAt(const std::vector<uint8_t> &img, uint32_t site, uint32_t target) {
	return img[site - IMAGE_BASE] == 0xE8 && site + 5 + Dword(img, site + 1) == target;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<uint8_t> b) {
	size_t i = 0;
	for (uint8_t x : b)
		if (img[va - IMAGE_BASE + i++] != x)
			return false;
	return true;
}

void TestAgainstTheImage() {
	std::printf("\nthe speed notes against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the bump "
		            "against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(Dword(img, CAutomobile__vtable_ProcessControl) == CAutomobile__ProcessControl,
	      "CAutomobile's slot 8 is its ProcessControl");
	Check(Bytes(img, 0x004B1B97, {0x8B, 0x39, 0xFF, 0x57, 0x20}) &&
	          Bytes(img, 0x004B1BE2, {0x8B, 0x39, 0xFF, 0x57, 0x20}) &&
	          Bytes(img, 0x004B1C32, {0x8B, 0x39, 0xFF, 0x57, 0x24}),
	      "CWorld::Process runs slot 8 over the list, the postponed again, then slot 9");
	Check(CallsAt(img, VEHICLE_DAMAGE_CALL, CAutomobile__VehicleDamage) &&
	          CallsAt(img, 0x00532B02, CPhysical__ProcessControl) &&
	          VEHICLE_DAMAGE_CALL < 0x00532B02,
	      "ProcessControl dents off the record before CPhysical::ProcessControl clears it");
	Check(Bytes(img, 0x00495F6F, {0x66, 0xC7, 0x83, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x00495F78, {0xC7, 0x83, 0x0C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x00495F82, {0xC7, 0x83, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}),
	      "which clears the piece, the impulse and the entity at +120h, +10Ch, +110h");
	Check(Bytes(img, 0x0052F3BF, {0xD9, 0x85, 0x0C, 0x01, 0x00, 0x00}) &&
	          Bytes(img, 0x0052F3CF, {0x66, 0x8B, 0xBD, 0x20, 0x01, 0x00, 0x00}) &&
	          offs::VEH_DAMAGE_IMPULSE == 0x10C && offs::VEH_DAMAGE_PIECE_TYPE == 0x120,
	      "VehicleDamage reads the impulse and the piece a bump writes");
}

} // namespace

int RunBumpTests() {
	g_bumpFailures = 0;
	TestTheWire();
	TestTheRammersSide();
	TestTheOwnersSide();
	TestTheSpeedNotes();
	TestAgainstTheImage();
	return g_bumpFailures;
}
