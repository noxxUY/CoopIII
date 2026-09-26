// What the owner's mission does to the streets, says out loud and draws, on
// every participant's machine, and how much less of it goes out than it runs
// (game/replay.h, game/effectshape.h, game/missionworld.h).
//
// The pure parts run everywhere. What the engine has to agree with is checked
// against the retail gta3.exe when COOPIII_GTA3_EXE names one, the way
// missions.cpp checks the rest of the script engine.

#include "game/effectshape.h"
#include "game/mission.h"
#include "game/missionworld.h"
#include "game/replay.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;

namespace {

int g_worldFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_worldFailures;
}

// An effect the way Record would have made it out of an instruction whose
// operands are all literals.
struct Fx {
	MissionEffectBody b{};
	Fx(uint8_t kind, uint16_t opcode) {
		b.kind      = kind;
		b.handleAt  = 0xFF;
		b.ownerBlip = -1;
		b.code[0]   = static_cast<uint8_t>(opcode & 0xFF);
		b.code[1]   = static_cast<uint8_t>(opcode >> 8);
		b.length    = 2;
	}
	Fx &Int(int32_t v) {
		b.code[b.length] = game::scripts::PARAM_INT32;
		std::memcpy(b.code + b.length + 1, &v, 4);
		b.length = static_cast<uint8_t>(b.length + 5);
		return *this;
	}
	Fx &Float(float f) {
		int32_t v = 0;
		std::memcpy(&v, &f, 4);
		return Int(v);
	}
	Fx &Handle(int32_t v) {
		b.handleAt  = static_cast<uint8_t>(b.length + 1);
		b.ownerBlip = v;
		return Int(v);
	}
	Fx &Text(const char *label) {
		char l[8] = {};
		std::strncpy(l, label, 8);
		std::memcpy(b.code + b.length, l, 8);
		b.length = static_cast<uint8_t>(b.length + 8);
		return *this;
	}
	Fx &Local0() {
		b.code[b.length]     = game::scripts::PARAM_LOCAL;
		b.code[b.length + 1] = 0;
		b.code[b.length + 2] = 0;
		b.length             = static_cast<uint8_t>(b.length + 3);
		return *this;
	}
	Fx &Made(int32_t handle) {
		b.ownerBlip = handle;
		return *this;
	}
};

uint16_t Op(const MissionEffectBody &b) { return game::shape::OpcodeOf(b); }

int32_t HandleIn(const MissionEffectBody &b) {
	int32_t v = -1;
	if (b.handleAt != 0xFF && b.handleAt + 4u <= b.length)
		std::memcpy(&v, b.code + b.handleAt, 4);
	return v;
}

// ---- the replay list's new instructions ----------------------------------------------

void TestTheStreetsAndTheWordsAreOnTheList() {
	std::printf("\nwhat a mission does to the streets and says, on the replay list\n");
	using namespace game::replay;
	std::vector<uint8_t> space(0x400, 0);

	// 03_rc1.sc: SET_ZONE_CAR_INFO 'TOWERS' DAY 8 0 0 200 0 0 0 0 20 400 0 0 350 0 0,
	// as the compiler writes it: the label, then sixteen 8- and 16-bit numbers.
	std::vector<uint8_t> zone = {'T', 'O', 'W', 'E', 'R', 'S', 0, 0};
	const int16_t        values[16] = {1, 8, 0, 0, 200, 0, 0, 0, 0, 20, 400, 0, 0, 350, 0, 0};
	for (int16_t v : values) {
		if (v >= -128 && v <= 127) {
			zone.push_back(game::scripts::PARAM_INT8);
			zone.push_back(static_cast<uint8_t>(v));
		} else {
			zone.push_back(game::scripts::PARAM_INT16);
			zone.push_back(static_cast<uint8_t>(v & 0xFF));
			zone.push_back(static_cast<uint8_t>(v >> 8));
		}
	}
	std::memcpy(space.data() + 0x100, zone.data(), zone.size());
	Encoded e;
	Check(Encode(0x0152, space.data(), 0x400, 0x100, nullptr, &e) && e.kind == Kind::World,
	      "SET_ZONE_CAR_INFO is on the list, and the campaign's: a save keeps the zones");
	Check(e.length == 2 + 8 + 16 * 3 && e.length <= MISSION_EFFECT_CODE && e.code[10] == 0x05 &&
	          e.code[11] == 1 && e.code[12] == 0,
	      "its sixteen numbers go as 16-bit literals, which is the only way they fit");
	int16_t fifth = 0;
	std::memcpy(&fifth, e.code + 10 + 4 * 3 + 1, 2);
	Check(fifth == 200, "each one the value the owner's engine read");
	std::memcpy(space.data() + 0x40, "\x00\x00\x01\x00", 4);   // a global of 65536
	std::vector<uint8_t> tooBig = zone;
	tooBig[8]                   = 0x02;   // the first number from that global
	tooBig[9]                   = 0x40;
	tooBig.insert(tooBig.begin() + 10, 0x00);
	std::memcpy(space.data() + 0x200, tooBig.data(), tooBig.size());
	Check(!Encode(0x0152, space.data(), 0x400, 0x200, nullptr, &e),
	      "and one that does not fit sixteen bits is not sent at all, not cut short");

	Check(Find(0x015C) && Find(0x015C)->kind == Kind::World && Find(0x015C)->count == 11 &&
	          Find(0x0237) && Find(0x0237)->kind == Kind::World,
	      "the gangs in a zone and a gang's weapons are the campaign's too");
	bool plain = true;
	for (uint16_t op : {0x0395, 0x03DE, 0x01EB, 0x03CF, 0x03D1, 0x03D7, 0x040D, 0x014D, 0x0384, 0x03BD,
	                    0x03CB, 0x0434, 0x0435})
		plain = plain && Find(op) && Find(op)->kind == Kind::Plain;
	Check(plain, "clearing an area, the densities, the mission's lines, the pager, the fire truck's "
	             "area, a sphere down, the scene loaded and the credits are shown to everybody");
	Check(Find(0x0162) && Find(0x0162)->kind == Kind::BlipNew && Find(0x0162)->args[0] == Arg::Char,
	      "Arms Shortage's attackers' old-style blips are blips, for the session's name of each");
	Check(Find(0x0255) && Find(0x0255)->kind == Kind::Teleport,
	      "a critical mission's restart moves everybody, each beside the spot");
	Check(!Find(0x024F) && !Find(0x03D0) && !Find(0x03D2),
	      "a corona is not run once, and the audio's questions are the owner's to ask");

	// ADD_SPHERE 925.0625 -350.5 9.25 2.5 -> $SPHERE, then its REMOVE_SPHERE.
	const uint8_t sphere[] = {0x06, 0x81, 0x39, 0x06, 0x18, 0xEA, 0x06, 0x94, 0x00, 0x06, 0x28, 0x00,
	                          0x02, 0x80, 0x00};
	std::memcpy(space.data() + 0x280, sphere, sizeof sphere);
	Check(Encode(0x03BC, space.data(), 0x400, 0x280, nullptr, &e) && e.kind == Kind::SphereNew &&
	          e.length == 2 + 4 * 5 + 3 && e.code[22] == game::scripts::PARAM_LOCAL,
	      "a sphere is made into the runner's local 0, like a blip");
	const int32_t ownerSphere = 0x00030002;
	std::memcpy(space.data() + 0x80, &ownerSphere, 4);
	const uint8_t remove[] = {0x02, 0x80, 0x00};
	std::memcpy(space.data() + 0x2A0, remove, sizeof remove);
	Check(Encode(0x03BD, space.data(), 0x400, 0x2A0, nullptr, &e), "and taken down by its handle");
	BlipMap spheres;
	Handles h;
	h.spheres = &spheres;
	Encoded run;
	Check(!Translate(e, h, &run), "one this machine never put up is dropped");
	spheres.Add(ownerSphere, 0x00010005);
	int32_t ours = 0;
	Check(Translate(e, h, &run) && (std::memcpy(&ours, run.code + 3, 4), ours == 0x00010005),
	      "and ours goes in the owner's place for one it did");
	Check(game::world::SphereSlot(0x00010005) == 5 && game::world::SphereSlot(-1) == -1 &&
	          game::world::SphereSlot(0x00010010) == -1,
	      "a sphere's slot is its handle's low word, and there are sixteen");

	// The standing picture for somebody who comes in: a zone set twice is the
	// last, a density is one, a sphere comes and goes.
	game::StandingEffects s;
	MissionEffectBody     z1{}, z2{}, other{};
	z1.kind = z2.kind = other.kind = MISSION_EFFECT_RUN;
	std::memcpy(space.data() + 0x100, zone.data(), zone.size());
	Encode(0x0152, space.data(), 0x400, 0x100, nullptr, &e);
	z1.length = e.length;
	std::memcpy(z1.code, e.code, e.length);
	z2         = z1;
	z2.code[14] = 99;   // a different car density
	other      = z1;
	other.code[2] = 'L';   // another zone
	s.Note(z1);
	s.Note(z2);
	s.Note(other);
	Check(s.Count() == 2 && s.At(0).code[14] == 99,
	      "a late joiner is handed each zone's last word and nothing it replaced");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x03DE).Float(0.0f).b);
	s.Note(Fx(MISSION_EFFECT_RUN, 0x03DE).Float(1.0f).b);
	float density = 0.0f;
	std::memcpy(&density, s.At(s.Count() - 1).code + 3, 4);
	Check(s.Count() == 3 && density == 1.0f, "and the density as it is now");
	s.Note(Fx(MISSION_EFFECT_SPHERE_NEW, 0x03BC).Float(1).Float(2).Float(3).Float(2.5f).Local0().Made(77).b);
	Check(s.Count() == 4, "a sphere up stands");
	s.Note(Fx(MISSION_EFFECT_RUN, 0x03BD).Int(77).b);
	Check(s.Count() == 3, "and taken down, it does not");
}

// ---- repeats ---------------------------------------------------------------------------

void TestARepeatIsNotSentTwice() {
	std::printf("\nwhat a mission says again and again goes out once, and its last word always\n");
	using namespace game::shape;
	std::vector<MissionEffectBody> out;
	auto                           send = [&](const MissionEffectBody &b) { out.push_back(b); };

	EffectShaper s;
	// SET_PLAYER_CONTROL 0 0 every frame for a second.
	for (uint32_t t = 1000; t < 2000; t += 16)
		s.Offer(Fx(MISSION_EFFECT_RUN, 0x01B4).Int(0).Int(0).b, t, send);
	Check(out.size() == 1 && s.Dropped() > 50, "the same setting every frame goes once");
	out.clear();
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x01B4).Int(0).Int(1).b, 2000, send);
	Check(out.size() == 1, "and a change goes at once after a quiet spell");

	// A density turned every frame goes at most ten times a second, and the
	// last of it is never lost.
	out.clear();
	for (uint32_t i = 0; i < 60; ++i) {
		const uint32_t t = 3000 + i * 16;
		s.Offer(Fx(MISSION_EFFECT_RUN, 0x03DE).Float(static_cast<float>(i) / 60.0f).b, t, send);
		s.Tick(t, send);
	}
	const size_t paced = out.size();
	Check(paced >= 5 && paced <= 11, "a setting changed every frame goes ten times a second at most");
	s.Tick(3000 + 60 * 16 + STATE_GAP_MS, send);
	float last = -1.0f;
	std::memcpy(&last, out.back().code + 3, 4);
	Check(out.size() == paced + 1 && last == 59.0f / 60.0f && s.Pending() == 0,
	      "and once the gap is up, the last value goes");

	// Money, a new blip, a teleport: never held, never dropped.
	out.clear();
	for (int i = 0; i < 3; ++i) {
		s.Offer(Fx(MISSION_EFFECT_PAY, 0x0109).Int(0).Int(100).b, 5000, send);
		s.Offer(Fx(MISSION_EFFECT_RUN, 0x020C).Float(1).Float(2).Float(3).Int(0).b, 5000, send);   // ADD_EXPLOSION
		s.Offer(Fx(MISSION_EFFECT_TELEPORT, 0x0055).Int(0).Float(1).Float(2).Float(3).b, 5000, send);
	}
	Check(out.size() == 9, "pay, explosions and moves are events: every one goes");

	// The words: said again the same, they go when they would otherwise run out.
	out.clear();
	for (uint32_t t = 6000; t < 9000; t += 16)
		s.Offer(Fx(MISSION_EFFECT_RUN, 0x00BC).Text("F_START").Int(5000).Int(1).b, t, send);
	Check(out.size() == 2, "PRINT_NOW for five seconds every frame goes every two seconds, not every frame");
	out.clear();
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x00BC).Text("OTHER").Int(5000).Int(1).b, 9500, send);
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x00BC).Text("F_START").Int(5000).Int(1).b, 9700, send);
	Check(out.size() == 2, "but one print over another, and back, is shown as it went");
	out.clear();
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x03D5).Text("F_START").b, 9800, send);
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x00BC).Text("F_START").Int(5000).Int(1).b, 9900, send);
	Check(out.size() == 2 && Op(out[1]) == 0x00BC, "and taken off, it is said afresh");

	// A blip changed, then taken away: what is held about it goes with it.
	out.clear();
	s.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0165).Handle(40).Int(1).b, 10000, send);
	s.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0165).Handle(40).Int(2).b, 10010, send);
	Check(out.size() == 1 && s.Pending() == 1, "a quick second colour waits its turn");
	s.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0164).Handle(40).b, 10020, send);
	s.Tick(10500, send);
	Check(out.size() == 2 && Op(out[1]) == 0x0164 && s.Pending() == 0,
	      "and a blip taken away takes its waiting colour with it");
	out.clear();
	s.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0165).Handle(40).Int(1).b, 11000, send);
	Check(out.size() == 1, "a blip made again under that handle hears its colour again");

	// An object moved every frame, then slid: its last place goes first.
	out.clear();
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x01BC).Int(0).Float(1).Float(2).Float(3).b, 12000, send);
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x01BC).Int(0).Float(1).Float(2).Float(4).b, 12016, send);
	Check(out.size() == 1 && s.Pending() == 1, "an object put somewhere twice in a frame waits");
	s.Flush(12020, send);
	Check(out.size() == 2 && s.Pending() == 0, "and the mission's end sends whatever still waits");

	// A setting said again the same, a second on: said again, since the
	// participant's own engine may have undone it (a cutscene's end takes the
	// bars off).
	out.clear();
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x02A3).Int(1).b, 14000, send);
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x02A3).Int(1).b, 14500, send);
	s.Offer(Fx(MISSION_EFFECT_RUN, 0x02A3).Int(1).b, 14000 + STATE_REFRESH_MS, send);
	Check(out.size() == 2, "the same setting a second later goes again");
	// Ray's lines, each loaded, placed at Phil's and played: the place again
	// after a load, which puts the line back in the ear.
	out.clear();
	for (uint32_t t = 15000; t < 15300; t += 100) {
		s.Offer(Fx(MISSION_EFFECT_RUN, 0x03CF).Text("R2_A").b, t, send);
		s.Offer(Fx(MISSION_EFFECT_RUN, 0x03D7).Float(1).Float(2).Float(3).b, t, send);
	}
	Check(out.size() == 6, "a line's place is said again after every load");

	// Somebody's own instruction is never shaped.
	out.clear();
	MissionEffectBody alone = Fx(MISSION_EFFECT_RUN, 0x01B4).Int(0).Int(1).b;
	alone.onlyTo            = 2;
	s.Offer(alone, 13000, send);
	s.Offer(alone, 13000, send);
	Check(out.size() == 2, "what one player alone is handed goes every time");
}

// ---- S.A.M.'s blip ---------------------------------------------------------------------

void TestSamsPlaneIsOneBlip() {
	std::printf("\nS.A.M.'s plane blip, taken off and put back every frame, is one blip that moves\n");
	using namespace game::shape;
	std::vector<MissionEffectBody> wire;
	EffectShaper                   shaper;
	BlipAliases                    aliases;
	uint32_t                       now = 0;
	auto                           toWire = [&](const MissionEffectBody &b) { wire.push_back(b); };
	auto shaped = [&](const MissionEffectBody &b) { shaper.Offer(b, now, toWire); };

	// Frame 1: the blip made, then made bigger.
	uint32_t frame  = 1;
	int32_t  handle = 0x00010003;
	now             = 1000;
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_NEW, 0x0167).Float(100).Float(0).Float(50).Int(4).Int(2).Local0().Made(handle).b,
	              frame, now, shaped);
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0168).Handle(handle).Int(3).b, frame, now, shaped);
	Check(wire.size() == 2 && Op(wire[0]) == 0x0167 && wire[0].ownerBlip == handle,
	      "the first blip goes out as it is");
	const int32_t first = handle;
	// Sixty frames, a second of LOOP_AS3_2: off, on where the plane is, bigger.
	for (int i = 0; i < 60; ++i) {
		++frame;
		now += 16;
		aliases.Tick(frame, now, shaped);
		shaper.Tick(now, toWire);
		aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0164).Handle(handle).b, frame, now, shaped);
		handle += 0x00010000;
		aliases.Offer(Fx(MISSION_EFFECT_BLIP_NEW, 0x0167)
		                  .Float(100.0f + static_cast<float>(i))
		                  .Float(0)
		                  .Float(50)
		                  .Int(4)
		                  .Int(2)
		                  .Local0()
		                  .Made(handle)
		                  .b,
		              frame, now, shaped);
		aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0168).Handle(handle).Int(3).b, frame, now, shaped);
	}
	Check(wire.size() <= 2 + 3 * 2 && aliases.Kept() > 50,
	      "a second of it is at most two moves of three packets, not 180");
	bool same = true;
	for (size_t i = 2; i < wire.size(); ++i)
		same = same && (wire[i].ownerBlip == first) && (HandleIn(wire[i]) == first || wire[i].kind == MISSION_EFFECT_BLIP_NEW);
	Check(same, "everybody keeps knowing it by the owner's first handle for it");
	Check(wire.size() >= 5 && Op(wire[2]) == 0x0164 && Op(wire[3]) == 0x0167 && Op(wire[4]) == 0x0168,
	      "a move is: off, on where it is now, and made bigger again");
	Check(aliases.WireOf(handle) == first, "and the owner's newest handle is the first one on the wire");

	// It stops moving: the last place goes once its time is up, then nothing.
	wire.clear();
	for (int i = 0; i < 60; ++i) {
		++frame;
		now += 16;
		aliases.Tick(frame, now, shaped);
		shaper.Tick(now, toWire);
		aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0164).Handle(handle).b, frame, now, shaped);
		handle += 0x00010000;
		aliases.Offer(Fx(MISSION_EFFECT_BLIP_NEW, 0x0167).Float(200).Float(0).Float(50).Int(4).Int(2).Local0().Made(handle).b,
		              frame, now, shaped);
		aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0168).Handle(handle).Int(3).b, frame, now, shaped);
	}
	float x = 0.0f;
	for (const MissionEffectBody &b : wire)
		if (Op(b) == 0x0167)
			std::memcpy(&x, b.code + 3, 4);
	Check(wire.size() == 3 && x == 200.0f, "where it stopped goes once, and then nothing more");

	// Shot down: taken off and not put back. It goes a frame later.
	wire.clear();
	++frame;
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0164).Handle(handle).b, frame, now, shaped);
	aliases.Tick(frame + 1, now + 16, shaped);
	Check(wire.empty(), "taken off, it waits a frame for the instruction that might put it back");
	aliases.Tick(frame + 2, now + 32, shaped);
	Check(wire.size() == 1 && Op(wire[0]) == 0x0164 && HandleIn(wire[0]) == first,
	      "and then goes from everybody's radar, under the handle they know");

	// Another blip of the same kind put up after a different one was taken
	// away is still just that blip where it is; one for another car is not.
	wire.clear();
	aliases.Clear();
	shaper.Clear();
	now += 5000;
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_NEW, 0x0186).Int(11).Local0().Made(500).b, 100, now, shaped);
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0164).Handle(500).b, 101, now, shaped);
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_NEW, 0x0186).Int(12).Local0().Made(501).b, 101, now, shaped);
	aliases.Tick(103, now, shaped);
	Check(wire.size() == 3 && wire[1].ownerBlip == 501 && Op(wire[2]) == 0x0164 && HandleIn(wire[2]) == 500,
	      "a car's blip moved to another car is a new blip, and the old one goes");

	// The mission ends mid-move: where it was going is where it ends up.
	wire.clear();
	aliases.Clear();
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_NEW, 0x018A).Float(1).Float(1).Float(1).Local0().Made(600).b, 200, now, shaped);
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_USE, 0x0164).Handle(600).b, 201, now + 16, shaped);
	aliases.Offer(Fx(MISSION_EFFECT_BLIP_NEW, 0x018A).Float(9).Float(9).Float(9).Local0().Made(601).b, 201, now + 16, shaped);
	Check(wire.size() == 1, "a move right after it was made waits its turn");
	aliases.Flush(now + 20, shaped);
	std::memcpy(&x, wire.back().code + 3, 4);
	Check(wire.size() == 3 && x == 9.0f && wire.back().ownerBlip == 600,
	      "and the mission's end puts it where it last was");
}

// ---- coronas ---------------------------------------------------------------------------

void TestTheCoronasGoToEverybody() {
	std::printf("\nthe coronas the owner's mission draws every frame are drawn on everybody's screen\n");
	using namespace game::shape;
	// 07_4x4_1.sc: DRAW_CORONA 1.0 HEX NONE 0 200 200 at the checkpoint.
	CoronaDraw c;
	c.x = -1100.5f, c.y = 350.25f, c.z = 30.0f, c.size = 1.0f, c.type = 1, c.flare = 0, c.r = 0, c.g = 200,
	c.b = 200;
	const MissionEffectBody up = CoronaEffect(7, 0x2A10, c, true);
	uint32_t   id = 0;
	CoronaDraw got;
	bool       gotUp = false;
	Check(up.kind == MISSION_EFFECT_RUN && up.length == CORONA_EFFECT_LENGTH &&
	          up.length <= MISSION_EFFECT_CODE && Op(up) == 0x024F,
	      "a corona goes as DRAW_CORONA, its nine operands and whether it is up");
	Check(ReadCoronaEffect(up, &id, &got, &gotUp) && gotUp && id == 0x2A10 && got.x == c.x && got.y == c.y &&
	          got.z == c.z && got.size == c.size && got.type == 1 && got.g == 200 && got.b == 200,
	      "and reads back as the same corona");
	Check(ReadCoronaEffect(CoronaEffect(7, 0x2A10, c, false), &id, &got, &gotUp) && !gotUp,
	      "down is down");
	Check(!ReadCoronaEffect(game::MarkerEffect(7, 1, game::MarkerArea{}, true), &id, &got, &gotUp),
	      "and a blue marker is not one");

	OwnCoronas own;
	int        told = 0, downs = 0;
	auto tell = [&](uint32_t, const CoronaDraw &, bool u, bool) { u ? ++told : ++downs; };
	for (uint32_t t = 1000; t < 1000 + CORONA_RESEND_MS + 100; t += 16) {
		own.Drawn(0x2A10, c, t);
		own.Tick(t, tell);
	}
	Check(told == 2, "drawn every frame for two seconds, it is told twice: up, and once again");
	CoronaDraw next = c;
	next.x += 50.0f;
	own.Drawn(0x2A10, next, 4000);
	own.Tick(4000, tell);
	Check(told == 3, "the next checkpoint is said as soon as it is a quarter second on");
	own.Tick(4000 + CORONA_GONE_MS, tell);
	Check(downs == 1 && own.Count() == 0, "and not drawn for 400 ms, it is out on every screen");

	ShownCoronas shown;
	int          drawn = 0;
	shown.Heard(0x2A10, c, true, 5000);
	shown.Each(5016, [&](uint32_t, const CoronaDraw &) { ++drawn; });
	shown.Each(5032, [&](uint32_t, const CoronaDraw &) { ++drawn; });
	Check(drawn == 2, "a participant draws it every frame");
	shown.Heard(0x2A10, c, false, 5100);
	Check(shown.Count() == 0, "until it hears it went out");
}

void TestChaperonesClubIsLitForEverybody() {
	std::printf("\nChaperone's club light and shadow are drawn on everybody's screen\n");
	using namespace game::shape;
	// 35_frank1.sc: DRAW_SHADOW SHADOW_EXPLOSION at 1270.813 -1107.688 11.0625
	// rotation 0.0 scale 1.0 transparency 0 colour 255 0 0, and DRAW_LIGHT at
	// 1273.188 -1107.25 11.0625 in red.
	const int32_t shadowOps[10] = {3, FloatBits(1270.813f), FloatBits(-1107.688f), FloatBits(11.0625f),
	                               FloatBits(0.0f), FloatBits(1.0f), 0, 255, 0, 0};
	const int32_t lightOps[6]   = {FloatBits(1273.188f), FloatBits(-1107.25f), FloatBits(11.0625f),
	                               255, 0, 0};
	CoronaDraw shadow, light;
	Check(FrameDrawOperands(DRAW_SHADOW) == 10 && FrameDrawOperands(DRAW_LIGHT) == 6 &&
	          FrameDrawOperands(DRAW_CORONA) == 9 && FrameDrawOperands(0x0250 + 1) == 0,
	      "a shadow takes ten operands, a light six and a corona nine");
	Check(FrameDrawFrom(DRAW_SHADOW, shadowOps, 10, &shadow) && shadow.type == 3 &&
	          shadow.x == 1270.813f && shadow.size == 1.0f && shadow.r == 255 &&
	          FrameDrawFrom(DRAW_LIGHT, lightOps, 6, &light) && light.x == 1273.188f && light.r == 255,
	      "each is read in the order the instruction takes its operands");
	Check(!FrameDrawFrom(DRAW_LIGHT, lightOps, 5, &light), "and not from too few");

	uint32_t   id = 0;
	CoronaDraw got;
	bool       up = false;
	const MissionEffectBody s = CoronaEffect(35, 0x3C00, shadow, true);
	Check(Op(s) == DRAW_SHADOW && s.length == 2 + 11 * 5 && s.length <= MISSION_EFFECT_CODE &&
	          ReadCoronaEffect(s, &id, &got, &up) && up && id == 0x3C00 && got.opcode == DRAW_SHADOW &&
	          got.y == shadow.y && got.flare == 0 && got.r == 255,
	      "a shadow goes as DRAW_SHADOW, its ten operands and whether it is up, and reads back");
	const MissionEffectBody l = CoronaEffect(35, 0x3C40, light, false);
	Check(Op(l) == DRAW_LIGHT && l.length == 2 + 7 * 5 && ReadCoronaEffect(l, &id, &got, &up) && !up &&
	          got.opcode == DRAW_LIGHT && got.z == light.z,
	      "and a light as DRAW_LIGHT");

	uint8_t      code[64];
	const size_t n = FrameDrawCode(light, code, sizeof code);
	int32_t      g = -1;
	std::memcpy(&g, code + 3 + 4 * 5, 4);
	Check(n == 2 + 6 * 5 && code[0] == 0x50 && code[1] == 0x02 && code[2] == game::scripts::PARAM_INT32 &&
	          g == 0,
	      "a participant runs the light itself, its operands as literals, with no flag after them");
	CoronaDraw moved = shadow;
	moved.angle      = 1.0f;
	Check(CoronaChanged(shadow, moved) && CoronaChanged(shadow, light) && !CoronaChanged(shadow, shadow),
	      "a shadow turned, or a light where a shadow was, is a change");

	OwnCoronas own;
	int        told = 0;
	auto       tell = [&](uint32_t, const CoronaDraw &, bool u, bool) { told += u ? 1 : 0; };
	for (uint32_t t = 1000; t < 1100; t += 16) {
		own.Drawn(0x3C00, shadow, t);
		own.Drawn(0x3C40, light, t);
		own.Tick(t, tell);
	}
	Check(told == 2 && own.Count() == 2, "drawn every frame, each is told once");
}

void TestTheSceneAndTheOwnersPlayer() {
	std::printf("\nthe owner's scenes on everybody's screen\n");
	using namespace game;
	Check(IsScriptedScene(true, false) && IsScriptedScene(false, true) && !IsScriptedScene(false, false),
	      "widescreen inside the mission hides the others as a cutscene does");
	Check(MayNameOwnPlayer(0x0159) && !MayNameOwnPlayer(0x0239) && !MayNameOwnPlayer(0x0157),
	      "only the camera on a ped may name the owner's player as each player's own");
	Check(WIRE_OWN_PLAYER > 0xFFFF, "and it is named past every netId");
}

// ---- against the image -------------------------------------------------------------------

bool LoadImage(std::vector<uint8_t> &image, std::string &from) {
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
		if (got == image.size() && image.size() == game::IMAGE_SIZE) {
			from = path;
			return true;
		}
	}
	return false;
}

uint8_t At(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = static_cast<size_t>(va - game::IMAGE_BASE);
	return va >= game::IMAGE_BASE && o < img.size() ? img[o] : 0;
}

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	return static_cast<uint32_t>(At(img, va)) | (static_cast<uint32_t>(At(img, va + 1)) << 8) |
	       (static_cast<uint32_t>(At(img, va + 2)) << 16) | (static_cast<uint32_t>(At(img, va + 3)) << 24);
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> pattern) {
	for (int b : pattern) {
		if (b >= 0 && At(img, va) != static_cast<uint8_t>(b))
			return false;
		++va;
	}
	return true;
}

bool Within(const std::vector<uint8_t> &img, uint32_t fn, uint32_t bytes, std::initializer_list<int> pattern) {
	for (uint32_t i = 0; i < bytes; ++i)
		if (Bytes(img, fn + i, pattern))
			return true;
	return false;
}

uint32_t CallAt(const std::vector<uint8_t> &img, uint32_t va) {
	return At(img, va) == 0xE8 ? va + 5 + Dword(img, va + 1) : 0;
}

// The handler of `op` in the range whose table and first opcode are given.
uint32_t Handler(const std::vector<uint8_t> &img, uint32_t table, int32_t first, int32_t op) {
	return Dword(img, table + 4 * static_cast<uint32_t>(op - first));
}

#define LE32(v)                                                                          \
	static_cast<int>((v) & 0xFF), static_cast<int>(((v) >> 8) & 0xFF),                  \
	    static_cast<int>(((v) >> 16) & 0xFF), static_cast<int>(((v) >> 24) & 0xFF)

void TestTheStreetsAgainstTheImage() {
	std::printf("\nthe streets, the lines, the spheres and the coronas against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadImage(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check them against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());
	namespace g = game;
	namespace w = game::world;
	const uint32_t params = g::CTheScripts__ScriptParams;
	const uint32_t t300 = g::g_ScriptOpcodeTable_300, t400 = g::g_ScriptOpcodeTable_400,
	               t500 = g::g_ScriptOpcodeTable_500, t900 = g::g_ScriptOpcodeTable_900,
	               t1000 = g::g_ScriptOpcodeTable_1000;

	// The handlers the replay list names, where it says they are.
	struct Named {
		uint32_t table;
		int32_t  first, op;
		uint32_t at;
	};
	const Named named[] = {
	    {t900, 900, w::op::CLEAR_AREA, 0x0044D84A},        {t900, 900, w::op::SET_PED_DENSITY_MULTIPLIER, 0x0044F730},
	    {t400, 400, w::op::SET_CAR_DENSITY_MULTIPLIER, 0x004426FA},
	    {t300, 304, w::op::SET_ZONE_CAR_INFO, 0x0043F27C}, {t300, 304, w::op::SET_ZONE_PED_INFO, 0x0043F65C},
	    {t500, 500, w::op::SET_GANG_WEAPONS, 0x00443F26},  {t900, 900, w::op::LOAD_MISSION_AUDIO, 0x0044F211},
	    {t900, 900, w::op::HAS_MISSION_AUDIO_LOADED, 0x0044F27C},
	    {t900, 900, w::op::PLAY_MISSION_AUDIO, 0x0044F2AB},
	    {t900, 900, w::op::SET_MISSION_AUDIO_POSITION, 0x0044F4C2},
	    {t1000, 1001, w::op::CLEAR_MISSION_AUDIO, 0x00588B93},
	    {t300, 304, w::op::ADD_PAGER_MESSAGE, 0x0043F12A}, {t900, 900, w::op::PRINT_STRING_IN_STRING, 0x0044CBAB},
	    {t300, 304, w::op::ADD_BLIP_FOR_CHAR_OLD, 0x0043F92A}, {t900, 900, w::op::ADD_SPHERE, 0x0044E927},
	    {t900, 900, w::op::REMOVE_SPHERE, 0x0044EA3D},     {t500, 500, w::op::DRAW_CORONA, 0x004447E9},
	    {t900, 900, w::op::LOAD_SCENE, 0x0044F0B6},        {t1000, 1001, w::op::START_CREDITS, 0x005896AA},
	    {t1000, 1001, w::op::STOP_CREDITS, 0x005896BA},    {t500, 500, w::op::RESTART_CRITICAL_MISSION, 0x004449E6},
	};
	int wrong = 0;
	for (const Named &n : named)
		if (Handler(img, n.table, n.first, n.op) != n.at) {
			std::printf("    %04X is at 0x%08X, not 0x%08X\n", static_cast<unsigned>(n.op),
			            Handler(img, n.table, n.first, n.op), n.at);
			++wrong;
		}
	Check(wrong == 0, "every new instruction's handler is where its comment says");

	// The streets.
	Check(Bytes(img, 0x0044F743, {0xD9, 0x05, LE32(params)}) &&
	          Within(img, 0x0044F743, 0x14, {0xD9, 0x1D, LE32(w::PED_DENSITY)}) &&
	          Bytes(img, 0x00442707, {0xD9, 0x05, LE32(params)}) &&
	          Within(img, 0x00442707, 0x14, {0xD9, 0x1D, LE32(w::CAR_DENSITY)}),
	      "each density instruction stores its one float where the density is kept");
	Check(CallAt(img, 0x0044D8DD) == 0x004B4E70 &&
	          Within(img, 0x004B4E70, 0x160, {0x8A, 0x85, 0xF5, 0x01, 0x00, 0x00, 0xC0, 0xE8, 0x03}) &&
	          CallAt(img, 0x004B4FC4) == 0x005511B0,
	      "CLEAR_AREA clears only what bIsLocked and CanBeDeleted let it: never a session car or a "
	      "player's");
	const uint32_t setCarInfo = CallAt(img, 0x0043F346);
	Check(setCarInfo == 0x004B6A50 &&
	          Within(img, setCarInfo, 0x40,
	                 {0x6B, 0xFF, static_cast<int>(w::ZONE_INFO_SIZE), 0x81, 0xC7, LE32(w::ZONE_INFO_ARRAY)}),
	      "SET_ZONE_CAR_INFO writes into an entry of 3Ah bytes of the array at 0x00714400");
	Check(Within(img, 0x004B5ED3, 0x180,
	             {0x83, 0xC0, static_cast<int>(w::ZONE_INFO_SIZE), 0x66, 0x83, 0xF9,
	              static_cast<int>(w::ZONE_INFOS)}) &&
	          Bytes(img, 0x004B5ED3, {0x66, 0xC7, 0x80, LE32(w::ZONE_INFO_ARRAY)}),
	      "which CTheZones::Init walks a hundred of");
	Check(CallAt(img, 0x00443F47) == 0x004C4030 &&
	          Bytes(img, 0x004C4039, {0xC1, 0xE1, 0x04, 0x81, 0xC1, LE32(w::GANGS), 0x89, 0x41,
	                                  static_cast<int>(w::GANG_WEAPON_1), 0x8B, 0x44, 0x24, 0x0C, 0x89, 0x41,
	                                  static_cast<int>(w::GANG_WEAPON_2)}) &&
	          Bytes(img, 0x004C4190, {0x6A, static_cast<int>(w::GANG_COUNT), 0x6A,
	                                  static_cast<int>(w::GANG_SIZE), 0x68, -1, -1, -1, -1, 0x68, LE32(w::GANGS)}),
	      "SET_GANG_WEAPONS writes a gang's two weapons at +8 and +0Ch of nine 10h-byte gangs");

	// The mission's line.
	const uint32_t dmAudio = 0x0095CDBE;
	Check(Bytes(img, 0x0044F27C, {0xB9, LE32(dmAudio)}) && CallAt(img, 0x0044F281) == 0x0057CD90 &&
	          Bytes(img, 0x0044F286, {0x3C, 0x01}),
	      "HAS_MISSION_AUDIO_LOADED takes no operand and asks DMAudio for a loading status of 1");
	Check(Bytes(img, 0x0044F2AB, {0xB9, LE32(dmAudio)}) && CallAt(img, 0x0044F2B0) == 0x0057CDE0 &&
	          Bytes(img, 0x00588B93, {0xB9, LE32(dmAudio)}) && CallAt(img, 0x00588B98) == 0x0057CE20 &&
	          Within(img, 0x0044F211, 0x60, {0xB9, LE32(dmAudio), 0x83, 0x40, 0x10, 0x08}),
	      "PLAY and CLEAR take none either, and LOAD takes its eight-byte name");

	// Spheres.
	const uint32_t addSphere = CallAt(img, 0x0044EA0E);
	Check(addSphere == 0x0044FB30 &&
	          Bytes(img, 0x0044FB44, {0x80, 0xB8, LE32(w::SCRIPT_SPHERES), 0x00}) &&
	          Bytes(img, 0x0044FB40, {0x83, 0xC0, static_cast<int>(w::SCRIPT_SPHERE)}) &&
	          Bytes(img, 0x0044FB4D, {0x83, 0xF9, static_cast<int>(w::SCRIPT_SPHERE_MAX)}) &&
	          Bytes(img, 0x0044FB67, {0xC6, 0x82, LE32(w::SCRIPT_SPHERES + w::SPHERE_IN_USE), 0x01}) &&
	          Bytes(img, 0x0044FB6E, {0x89, 0x82, LE32(w::SCRIPT_SPHERES + w::SPHERE_ID)}),
	      "ADD_SPHERE takes one of sixteen 18h-byte spheres, in use at +0 and its id at +4");
	Check(Within(img, 0x0044FAC0, 0x50, {0x8B, 0x85, LE32(w::SCRIPT_SPHERES + w::SPHERE_ID), 0x6A, 0x04, 0x50,
	                                     0xE8}),
	      "and the id is what the 3D marker each one is drawn as is known by");
	Check(Bytes(img, 0x0044FA6C, {0x0F, 0xB7, 0x80, LE32(w::SCRIPT_SPHERES + 2), 0xC1, 0xE0, 0x10, 0x09, 0xC8}),
	      "a sphere's handle is its slot with its count in the high word");

	// Coronas, and the ground.
	Check(Bytes(img, 0x004447EE, {0x6A, 0x09}) && CallAt(img, 0x004448B3) == w::CCoronas__RegisterCorona &&
	          Bytes(img, 0x004448B8, {0x83, 0xC4, 0x38}) &&
	          Bytes(img, 0x0044487F, {0xFF, 0x35, LE32(0x005EF1C8u)}) && Dword(img, 0x005EF1C8) == 0x43160000 &&
	          Bytes(img, 0x00444864, {0xFF, 0x35, LE32(0x005EF17Cu)}) && Dword(img, 0x005EF17C) == 0 &&
	          Bytes(img, 0x0044489E, {0x68, 0xFF, 0x00, 0x00, 0x00}),
	      "DRAW_CORONA collects nine and registers a corona with fourteen arguments: 255, 150.0 and "
	      "0.0 among them");
	float drawDistance = 0.0f;
	std::memcpy(&drawDistance, &w::CORONA_DRAW_DISTANCE, 4);
	Check(drawDistance == 150.0f, "which is the draw distance a participant registers with");
	Check(CallAt(img, 0x00444835) == w::CWorld__FindGroundZForCoord &&
	          CallAt(img, 0x0044D89C) == w::CWorld__FindGroundZForCoord &&
	          CallAt(img, 0x00444A3F) == w::CWorld__FindGroundZForCoord &&
	          Bytes(img, 0x0044483A, {0xD9, 0x9C, 0x24}) && Bytes(img, 0x00444841, {0x59, 0x59}),
	      "the ground under a coordinate is one cdecl call of two floats, the answer in st0");

	// The restart.
	Check(Bytes(img, 0x004449EB, {0x6A, 0x04}) && CallAt(img, 0x004449EE) == g::CTheScripts__CollectParameters,
	      "RESTART_CRITICAL_MISSION takes four, the place first");
}
#undef LE32

} // namespace

int RunMissionWorldTests() {
	TestTheStreetsAndTheWordsAreOnTheList();
	TestARepeatIsNotSentTwice();
	TestSamsPlaneIsOneBlip();
	TestTheCoronasGoToEverybody();
	TestChaperonesClubIsLitForEverybody();
	TestTheSceneAndTheOwnersPlayer();
	TestTheStreetsAgainstTheImage();
	return g_worldFailures;
}
