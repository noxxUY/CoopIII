// What another machine sends is checked before this one acts on it: an owner's
// instruction before our interpreter runs it (replay.h, WellFormed and
// OperandsInRange), a head that must not be made apart from its animation
// (missionsync.cpp, HoldsForNext), and the numbers a peer can make wild - a
// place, a speed, a rotation, a corona - before the engine is handed them.

#include "game/cutscene.h"
#include "game/effectshape.h"
#include "game/mission.h"
#include "game/pedanim.h"
#include "game/replay.h"
#include "missionsync.h"
#include "quat.h"

#include <coopiii/mission.h>
#include <coopiii/net.h>
#include <coopiii/protocol.h>

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

int g_wireFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_wireFailures;
}

constexpr uint32_t GLOBALS_END = 0x2000;

// An instruction as replay::Encode writes it, one operand at a time.
struct Code {
	uint8_t b[replay::MAX_CODE] = {};
	uint8_t n                   = 0;
	explicit Code(uint16_t opcode) { Byte(uint8_t(opcode & 0xFF)).Byte(uint8_t(opcode >> 8)); }
	Code &Byte(uint8_t v) {
		if (n < sizeof b)
			b[n++] = v;
		return *this;
	}
	Code &Int(int32_t v) {
		Byte(scripts::PARAM_INT32);
		for (int i = 0; i < 4; ++i)
			Byte(uint8_t(uint32_t(v) >> (8 * i)));
		return *this;
	}
	Code &Global(uint16_t g) { return Byte(scripts::PARAM_GLOBAL).Byte(uint8_t(g & 0xFF)).Byte(uint8_t(g >> 8)); }
	Code &Local(uint16_t i) { return Byte(scripts::PARAM_LOCAL).Byte(uint8_t(i & 0xFF)).Byte(uint8_t(i >> 8)); }
	Code &Text(const char *t, bool terminated = true) {
		char label[8] = {};
		std::memcpy(label, t, std::strlen(t) < 8 ? std::strlen(t) : 8);
		if (!terminated)
			for (char &c : label)
				if (c == '\0')
					c = 'X';
		for (char c : label)
			Byte(uint8_t(c));
		return *this;
	}
	bool Ok() const { return replay::WellFormed(b, n, GLOBALS_END); }
	bool InRange() const { return replay::OperandsInRange(b, n); }
};

// Every instruction on the list, read out of a script the way the owner's
// engine would hold it and encoded by replay::Encode: what the owner sends is
// what a participant accepts, all of it.
void TestEverythingEncodeWritesIsAccepted() {
	std::printf("\nan owner's instruction, as it is sent, is one a participant runs\n");
	size_t listed = 0, accepted = 0;
	uint16_t firstRefused = 0;
	for (uint32_t op = 0; op < 0x1000; ++op) {
		const replay::Entry *e = replay::Find(static_cast<uint16_t>(op));
		if (!e)
			continue;
		++listed;
		uint8_t space[256] = {};
		uint32_t at        = 16;
		for (uint8_t i = 0; i < e->count; ++i) {
			switch (e->args[i]) {
			case replay::Arg::Text:
				std::memcpy(space + at, "LABEL\0\0\0", 8);
				at += 8;
				break;
			case replay::Arg::Global:
			case replay::Arg::ObjGlobal:
			case replay::Arg::OutGlobal:
			case replay::Arg::OutBlip:
			case replay::Arg::SoundOut:
			case replay::Arg::SoundGlobal:
				space[at++] = scripts::PARAM_GLOBAL;
				space[at++] = 0x00;
				space[at++] = 0x01;
				break;
			case replay::Arg::Output:
				space[at++] = scripts::PARAM_LOCAL;
				space[at++] = 3;
				space[at++] = 0;
				break;
			case replay::Arg::Short:
				space[at++] = scripts::PARAM_INT16;
				space[at++] = 7;
				space[at++] = 0;
				break;
			default:
				space[at++] = scripts::PARAM_INT32;
				space[at++] = 1;
				at += 3;
				break;
			}
		}
		replay::Encoded enc;
		int32_t         locals[18] = {};
		if (replay::Encode(static_cast<uint16_t>(op), space, sizeof space, 16, locals, &enc) &&
		    replay::WellFormed(enc.code, enc.length, GLOBALS_END))
			++accepted;
		else if (firstRefused == 0)
			firstRefused = static_cast<uint16_t>(op);
	}
	if (firstRefused != 0)
		std::printf("  first refused: %04X\n", firstRefused);
	Check(listed > 150 && accepted == listed, "every opcode on the replay list, encoded, is well formed");

	// And the ones built by hand in their image.
	const uint8_t label[TEXT_LABEL] = {'G', 'B', '_', '1', 0, 0, 0, 0};
	const MissionEffectBody getBackIn = GetBackInEffect(19, label, 2);
	Check(replay::WellFormed(getBackIn.code, getBackIn.length, GLOBALS_END),
	      "the get-back-in line the owner prints for one player");
	const MissionEffectBody place = ObjectPlaceEffect(19, 0x100, 1.0f, 2.0f, 3.0f);
	const MissionEffectBody turn  = ObjectHeadingEffect(19, 0x100, 90.0f);
	Check(replay::WellFormed(place.code, place.length, GLOBALS_END) &&
	          replay::WellFormed(turn.code, turn.length, GLOBALS_END),
	      "an object's place and heading, handed to somebody who came in late");
	uint8_t model[CUTSCENE_MODEL_CODE];
	const size_t special = CutsceneModelCode(CUTSCENE_MI_SPECIAL01, "eight", model);
	Check(special != 0 && replay::WellFormed(model, special, GLOBALS_END),
	      "a scene's special character, asked for ahead of the scene");
	const size_t request = CutsceneModelCode(90, "kuruma", model);
	Check(request != 0 && replay::WellFormed(model, request, GLOBALS_END),
	      "and any other model it needs");
}

void TestWhatIsNotRun() {
	std::printf("\nwhat a participant's interpreter is never handed\n");
	Check(Code(0x02E4).Text("RAY1").Ok(), "LOAD_CUTSCENE with its name");
	Check(!Code(0x02E4).Text("RAY1_ABC", false).Ok(),
	      "a name with no end in its eight bytes, which the handler would copy on past");
	Check(Code(0x0107).Int(-3).Int(0).Int(0).Int(0).Global(0x100).Ok(), "CREATE_OBJECT into a global");
	Check(!Code(0x0107).Int(-3).Int(0).Int(0).Int(0).Global(4).Ok(),
	      "into a global below the variables");
	Check(!Code(0x0107).Int(-3).Int(0).Int(0).Int(0).Global(GLOBALS_END).Ok(),
	      "or past them, into main.scm's code");
	Check(Code(0x018A).Int(0).Int(0).Int(0).Local(0).Ok(), "ADD_BLIP_FOR_COORD into our local 0");
	Check(!Code(0x018A).Int(0).Int(0).Int(0).Local(0x4000).Ok(),
	      "into a local past our runner's sixteen, which StoreParameters writes all the same");
	Check(!Code(0x018A).Int(0).Local(9).Int(0).Local(0).Ok(),
	      "a value read out of a local, which may be anywhere past the runner");
	Check(!Code(0x018A).Int(0).Int(0).Int(0).Global(0x100).Ok(),
	      "a blip's handle stored in a global nobody asked for");
	Check(!Code(0x018A).Int(0).Int(0).Int(0).Local(0).Byte(0).Ok(), "a byte past the last operand");
	Check(!Code(0x018A).Int(0).Int(0).Int(0).Ok(), "a missing operand");
	Check(!Code(0x0002).Int(0x100).Ok(), "GOTO, which is not on the list");
	Check(!Code(0x004F).Int(0x100).Ok(), "START_NEW_SCRIPT, which is not either");
	Check(!replay::WellFormed(nullptr, 0, GLOBALS_END) &&
	          !replay::WellFormed(Code(0x00BE).b, 1, GLOBALS_END),
	      "nothing, and half an opcode");
	Check(Code(0x00BE).Ok(), "CLEAR_PRINTS, which has no operands");
	Check(Code(0x0164).Global(0x100).Ok() && Code(0x0164).Int(0x10001).Ok(),
	      "REMOVE_BLIP by the owner's handle, or by a contact's global");
	Check(!Code(0x0164).Global(2).Ok(), "but not by a global that is not one");
	Code zone(0x0152);
	zone.Text("SUB_IND");
	for (int i = 0; i < 16; ++i)
		zone.Byte(scripts::PARAM_INT16).Byte(1).Byte(0);
	Check(zone.Ok(), "SET_ZONE_CAR_INFO's sixteen shorts");
	Code wrongShort(0x0152);
	wrongShort.Text("SUB_IND");
	for (int i = 0; i < 16; ++i)
		wrongShort.Int(1);
	Check(!wrongShort.Ok(), "and not as anything else");
}

void TestWhatIsOutOfRange() {
	std::printf("\nan index the engine's handler would take into a table as it is\n");
	Check(Code(0x01B1).Int(0).Int(12).Int(100).InRange(), "GIVE_WEAPON_TO_PLAYER, the last weapon there is");
	Check(!Code(0x01B1).Int(0).Int(13).Int(100).InRange(), "one past m_weapons' thirteen");
	Check(!Code(0x01B1).Int(0).Int(-1).Int(100).InRange(), "or before them");
	Check(!Code(0x01B1).Int(1).Int(2).Int(100).InRange(), "or for player 1, who has no ped anywhere");
	Check(Code(0x0055).Int(0).Int(0).Int(0).Int(0).InRange() &&
	          !Code(0x0055).Int(3).Int(0).Int(0).Int(0).InRange(),
	      "SET_PLAYER_COORDINATES for player 0 only");
	Check(Code(0x0360).Int(31).InRange() && !Code(0x0360).Int(32).InRange(),
	      "OPEN_GARAGE inside aGarages' 32");
	Check(Code(0x021B).Int(5).Int(-1).InRange() && !Code(0x021B).Int(-1).Int(-1).InRange(),
	      "SET_TARGET_CAR_FOR_MISSION_GARAGE: no car is fine, no garage is not");
	Check(!Code(0x024C).Int(50).Text("AM4_1A").InRange(), "SET_PHONE_MESSAGE past the fifty phones");
	Check(Code(0x023C).Int(4).Text("eight").InRange() && !Code(0x023C).Int(5).Text("eight").InRange() &&
	          !Code(0x023C).Int(0).Text("eight").InRange(),
	      "LOAD_SPECIAL_CHARACTER in slots 1 to 4");
	Check(!Code(0x0437).Int(68).Int(0).Int(0).Int(0).Int(0).Int(0).Int(0).Int(0).InRange(),
	      "CREATE_SINGLE_PARTICLE past the particle table");
	Check(Code(0x01F9).Text("PAGE_00").Int(16).Int(0).Int(0).Int(0).Int(0).Int(0).Int(0).Int(0).InRange() &&
	          !Code(0x01F9).Text("PAGE_00").Int(-4).Int(0).Int(0).Int(0).Int(0).Int(0).Int(0).Int(0).InRange(),
	      "START_KILL_FRENZY with a cause that is no weapon, but not with a negative one");
	Check(Code(0x010D).Int(0).Global(0x100).InRange(),
	      "an operand sent as the machine's own global is that machine's to hold");
	Check(Code(0x00BA).Text("TM3_T").Int(5000).Int(1).InRange(), "an instruction with no table in it");

	bool used = false;
	Check(replay::ModelOperand(0x0107, &used) == 0 && used, "CREATE_OBJECT's model may be a used object");
	Check(replay::ModelOperand(0x02F4, &used) == 1 && !used, "CREATE_CUTSCENE_HEAD's is its second");
	Check(replay::ModelOperand(0x00BA, &used) == -1, "PRINT_BIG has none");
	Check(replay::BuildsFromModel(0x0107) && replay::BuildsFromModel(0x029B) &&
	          !replay::BuildsFromModel(0x0247),
	      "the two CREATE_OBJECTs build from their model; REQUEST_MODEL does not");
}

// ---- a head and its animation ---------------------------------------------------

std::vector<uint16_t> g_ran;
int                   g_ranInOneCall = 0;
MissionBridge         g_bridge;

S_MissionEffect Effect(uint8_t kind, uint16_t opcode) {
	S_MissionEffect fx;
	InitHeader(fx, 1000);
	fx.ownerId           = 0;
	fx.body.missionNumber = 19;
	fx.body.kind         = kind;
	fx.body.length       = 2;
	fx.body.code[0]      = uint8_t(opcode & 0xFF);
	fx.body.code[1]      = uint8_t(opcode >> 8);
	return fx;
}

S_MissionState Running() {
	S_MissionState s;
	InitHeader(s, 1000);
	s.checkpointWaitS = MISSION_CHECKPOINT_WAIT_MS / 1000;
	s.catchUpM        = MISSION_CATCH_UP_M_DEFAULT;
	s.behindM         = MISSION_BEHIND_M_DEFAULT;
	s.behindS         = MISSION_BEHIND_S_DEFAULT;
	s.state           = MISSION_STATE_RUNNING;
	s.ownerId         = 0;
	s.missionNumber   = 19;
	s.participants    = 0x03;
	s.flags           = MISSION_FLAG_FAIL_ON_DEATH;
	return s;
}

void Bind(MissionSync &m) {
	g_bridge           = MissionBridge{};
	g_bridge.RunEffect = [](const MissionEffectBody &b) {
		g_ran.push_back(uint16_t(b.code[0] | (b.code[1] << 8)));
		return true;
	};
	m.Bind(
	    &g_bridge, [](void *, const void *, size_t, Channel) {}, nullptr, [](void *, const char *) {},
	    [](void *, const char *) {}, [](void *, uint8_t) -> const char * { return "alice"; },
	    [](void *, MissionPresence *, size_t) -> size_t { return 0; }, nullptr);
	m.OnState(Running(), 1, 1000);
}

void TestAHeadIsMadeWithItsAnimation() {
	std::printf("\nCREATE_CUTSCENE_HEAD and SET_HEAD_ANIM, which the script runs together\n");
	MissionSync m;
	Bind(m);
	g_ran.clear();
	m.OnEffect(Effect(MISSION_EFFECT_OBJECT_NEW, 0x02F4), 1, false, 1000);
	Check(g_ran.empty() && m.EffectsAwaiting() == 1, "the head alone is not made");
	m.Tick(1, 1016);
	Check(g_ran.empty(), "not in the next frame either, where the world would walk it with no animation");
	m.OnEffect(Effect(MISSION_EFFECT_RUN, 0x02F5), 1, false, 1033);
	Check(g_ran.size() == 2 && g_ran[0] == 0x02F4 && g_ran[1] == 0x02F5 && m.EffectsAwaiting() == 0 &&
	          m.EffectsAwaited() == 0,
	      "its animation comes: both are run, in order, in the one call");

	g_ran.clear();
	m.OnEffect(Effect(MISSION_EFFECT_OBJECT_NEW, 0x02F4), 1, false, 2000);
	m.OnEffect(Effect(MISSION_EFFECT_RUN, 0x02F5), 1, false, 2000);
	Check(g_ran.size() == 2, "the two in the same drain run as they always did");

	g_ran.clear();
	m.OnEffect(Effect(MISSION_EFFECT_OBJECT_NEW, 0x02F4), 1, false, 3000);
	m.Tick(1, 3000 + MISSION_EFFECT_AWAIT_MS - 1);
	Check(g_ran.empty() && m.EffectsAwaiting() == 1, "a head whose animation is late waits for it");
	m.Tick(1, 3000 + MISSION_EFFECT_AWAIT_MS);
	Check(g_ran.empty() && m.EffectsAwaiting() == 0,
	      "and one whose animation never comes is never made");

	g_ran.clear();
	m.OnEffect(Effect(MISSION_EFFECT_OBJECT_NEW, 0x02E5), 1, false, 4000);
	Check(g_ran.size() == 1, "a scene's object is made at once: it is out of the world until the start");
	m.OnEffect(Effect(MISSION_EFFECT_RUN, 0x00BA), 1, false, 4000);
	Check(g_ran.size() == 2 && m.EffectsAwaiting() == 0, "and nothing else waits");
}

// ---- numbers a peer can make wild ------------------------------------------------

void TestWildNumbers() {
	std::printf("\nplaces, speeds, rotations and coronas off the wire\n");
	const float inf = std::numeric_limits<float>::infinity();
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Check(InsideWorld(890.0f, -310.0f, 12.0f) && InsideWorld(0.0f, 0.0f, -100.0f), "a street, and a z of find-the-ground");
	Check(!InsideWorld(nan, 0.0f, 0.0f) && !InsideWorld(0.0f, inf, 0.0f) && !InsideWorld(0.0f, 0.0f, nan),
	      "no number is no place");
	Check(!InsideWorld(1.0e9f, 0.0f, 0.0f) && !InsideWorld(0.0f, -5000.0f, 0.0f),
	      "nor is one the sector grid has no square for");
	Check(WireSpeed(0.3f) == 0.3f && WireSpeed(-0.3f) == -0.3f, "a ped's speed is written as it came");
	Check(WireSpeed(nan) == 0.0f && WireSpeed(inf) == 0.0f && WireSpeed(40.0f) == REMOTE_PED_SPEED_MAX &&
	          WireSpeed(-1.0e8f) == -REMOTE_PED_SPEED_MAX,
	      "a wild one is held to what a ped could be thrown at");

	Vec3 r, f, u;
	AxesFromQuat(Quat{inf, 0.0f, 0.0f, 1.0f}, r, f, u);
	Check(r.x == 1.0f && f.y == 1.0f && u.z == 1.0f, "an infinite quaternion is identity, not a NaN matrix");
	AxesFromQuat(Quat{3.0e19f, 3.0e19f, 0.0f, 0.0f}, r, f, u);
	Check(std::isfinite(r.x) && std::isfinite(f.y) && std::isfinite(u.z),
	      "and one whose length overflows is no NaN either");

	int32_t v[shape::FRAME_DRAW_MAX] = {};
	const float xyz[4] = {1.0f, 2.0f, 3.0f, 1.0f};
	for (int i = 0; i < 4; ++i)
		std::memcpy(&v[i], &xyz[i], 4);
	shape::CoronaDraw c;
	v[4] = 5;   // TYPE_HEX, which is every corona the campaign draws
	v[5] = 0;
	Check(shape::FrameDrawFrom(shape::DRAW_CORONA, v, 9, &c), "the campaign's own corona");
	v[4] = 9;
	Check(!shape::FrameDrawFrom(shape::DRAW_CORONA, v, 9, &c), "a sprite past gpCoronaTexture's nine");
	v[4] = 5;
	v[5] = 3;
	Check(!shape::FrameDrawFrom(shape::DRAW_CORONA, v, 9, &c),
	      "a flare the engine's switch leaves its pointer unset for");
	v[5] = -1;
	Check(!shape::FrameDrawFrom(shape::DRAW_CORONA, v, 9, &c), "or a negative one");
}

// ---- the bytes in the real exe ----------------------------------------------------

bool LoadExe(std::vector<uint8_t> &image) {
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
		if (got == image.size() && image.size() == IMAGE_SIZE)
			return true;
	}
	return false;
}

bool BytesAt(const std::vector<uint8_t> &img, uintptr_t va, std::initializer_list<uint8_t> bytes) {
	size_t i = 0;
	for (uint8_t b : bytes)
		if (img[va - IMAGE_BASE + i++] != b)
			return false;
	return true;
}

void TestTheBytesInTheImage() {
	std::printf("\nwhat the checks above read, in the retail exe\n");
	std::vector<uint8_t> img;
	if (!LoadExe(img)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the bytes\n");
		return;
	}
	// CREATE_OBJECT's handler, range 200's entry 49: a negative model read
	// out of UsedObjectArray with no test against the count.
	Check(BytesAt(img, 0x005EEC40 + 49 * 4, {0x20, 0xDD, 0x43, 0x00}) &&
	          BytesAt(img, 0x0043DD37, {0xF7, 0xDE, 0x8D, 0x04, 0xF5, 0x00, 0x00, 0x00, 0x00, 0x29, 0xF0,
	                                    0x8B, 0x34, 0x85, 0xE0, 0x69, 0x6E, 0x00}),
	      "CREATE_OBJECT reads UsedObjectArray[-model].index at 0x006E69E0, stride 1Ch, unchecked");
	Check(CTheScripts__UsedObjectArray + USED_OBJECT_INDEX == 0x006E69E0 && USED_OBJECT_STRIDE == 0x1C,
	      "which is where addresses.h has the table and its index");
	Check(BytesAt(img, 0x00438A40, {0x81, 0xF9, 0xC8, 0x00, 0x00, 0x00}) &&
	          BytesAt(img, 0x00438A4A, {0x66, 0xC7, 0x05, 0x72, 0xCC, 0x95, 0x00, 0x00, 0x00}) &&
	          BytesAt(img, 0x004549B0, {0x66, 0x89, 0x15, 0x72, 0xCC, 0x95, 0x00}),
	      "two hundred of them, counted by the word at 0x0095CC72 the loader sets");
	Check(BytesAt(img, 0x0040A284, {0x80, 0x78, 0x2A, MITYPE_PED}) &&
	          BytesAt(img, 0x0040A28F, {0x80, 0x7B, 0x2A, MITYPE_VEHICLE}),
	      "a ped's model info is type 6, beside a vehicle's 5");
}

} // namespace

int RunWireCheckTests() {
	g_wireFailures = 0;
	TestEverythingEncodeWritesIsAccepted();
	TestWhatIsNotRun();
	TestWhatIsOutOfRange();
	TestAHeadIsMadeWithItsAnimation();
	TestWildNumbers();
	TestTheBytesInTheImage();
	return g_wireFailures;
}
