// Shattered glass: client/src/game/glass.h, and the windscreen half of the car
// damage that already travels (game/cardamage.h).
//
// The four redirected calls cannot run here. What can is every decision they
// make, what a body off the wire is mended into, and, with a retail exe,
// every address and byte the glass half leans on.

#include "game/addresses.h"
#include "game/cardamage.h"
#include "game/driveby.h"
#include "game/glass.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_glassFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_glassFailures;
}

ObjectIdent Window() {
	ObjectIdent id{};
	id.pos        = {-21.5f, -1180.25f, 26.0f};
	id.modelIndex = 1390;
	return id;
}

void TestWhoShattersAndWhoSays() {
	std::printf("\nglass: who runs the shatter, and who says so\n");
	Check(GlassShattersHere(GlassSite::Collision, false) &&
	          GlassShattersHere(GlassSite::Collision, true),
	      "a collision shatters here whatever else is going on");
	Check(GlassShattersHere(GlassSite::Blast, true),
	      "and so does a blast, which every machine runs on the same numbers");
	Check(GlassShattersHere(GlassSite::Round, false),
	      "our own round rolls its one in four");
	Check(!GlassShattersHere(GlassSite::Round, true),
	      "somebody else's round does not: that roll is the shooter's");

	Check(GlassShatterGoesOut(false, true, false, true, true),
	      "a window that went from whole to broken here, in a session, goes out");
	Check(!GlassShatterGoesOut(true, true, false, true, true),
	      "one already broken says nothing (the engine did nothing to it)");
	Check(!GlassShatterGoesOut(false, false, false, true, true),
	      "nor a call that left it whole");
	Check(!GlassShatterGoesOut(false, true, true, true, true),
	      "nor one we were told about, so two machines never echo");
	Check(!GlassShatterGoesOut(false, true, false, false, true),
	      "nor one in single player");
	Check(!GlassShatterGoesOut(false, true, false, true, false),
	      "nor a window that is not the map's");
}

void TestTheLatch() {
	std::printf("\nglass: the bitfield byte the engine latches\n");
	Check(object::OBJ_GLASS_CRACKED == 0x08 && object::OBJ_GLASS_BROKEN == 0x10,
	      "cracked is bit 3 and broken bit 4 of +0x175");
	Check(!GlassIsBroken(0x08) && GlassIsBroken(0x10) && GlassIsBroken(0x19),
	      "broken is read off bit 4 alone");
	Check(GlassLatchedFlags(0x00) == 0x18 && GlassLatchedFlags(0x41) == 0x59,
	      "the latch sets cracked and broken and leaves the pickup and colour bits alone");

	const int16_t ids[8] = {387, 388, 389, 390, 394, 1390, 2791, 2792};
	Check(ModelIsGlass(1390, ids, 8) && ModelIsGlass(2792, ids, 8),
	      "the eight models are glass");
	Check(!ModelIsGlass(1393, ids, 8), "a lamp post is not");
	const int16_t unloaded[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
	Check(!ModelIsGlass(-1, unloaded, 8) && !ModelIsGlass(390, unloaded, 8),
	      "and before the game has loaded nothing is, not even model -1");
}

void TestTheWire() {
	std::printf("\nglass: the body on the wire\n");
	Check(sizeof(GlassBreakBody) == 48 && sizeof(C_GlassBroken) == 53 &&
	          sizeof(S_GlassBroken) == 54,
	      "48 bytes a window, once");
	Check(C_GlassBroken::OPCODE == 0xE6 && S_GlassBroken::OPCODE == 0xE7,
	      "on the free pair after the money block");
	Check((OBJ_BREAK_GLASS & (OBJ_BREAK_RENDER_DAMAGED | OBJ_BREAK_SMASHED |
	                          OBJ_BREAK_UPROOTED)) == 0,
	      "the record's glass bit is none of the street object bits");

	ObjectIdent id = Window();
	id.pad0        = 7;
	const GlassBreakBody body =
	    MakeGlassBody(id, 742.0f, Vec3{0.5f, 0.0f, 0.0f}, Vec3{-21.0f, -1180.0f, 27.0f}, true);
	Check(body.ident.pad0 == 0 && body.ident.modelIndex == 1390 && body.amount == 742.0f &&
	          body.flags == GLASS_BREAK_EXPLOSION,
	      "the sender's body is what the engine was handed, pads zeroed");

	GlassShatter s;
	Check(CleanGlassBody(body, s) && s.explosion && s.amount == 742.0f && s.speed.x == 0.5f &&
	          s.point.z == 27.0f,
	      "a sane body is run exactly as it came");

	const float nan = std::numeric_limits<float>::quiet_NaN();
	GlassBreakBody bad = body;
	bad.ident.pos.y    = nan;
	Check(!CleanGlassBody(bad, s), "a placement that is not a number names nothing");

	bad        = body;
	bad.amount = nan;
	bad.speed  = {nan, 0.0f, 0.0f};
	bad.point  = {0.0f, 0.0f, 0.0f};
	Check(CleanGlassBody(bad, s) && s.amount == 0.0f && s.speed.x == 0.0f &&
	          s.point.x == body.ident.pos.x && s.point.y == body.ident.pos.y,
	      "a broken amount is a round's 0, a broken speed is none, and a point "
	      "a kilometre off is the window's own placement");

	bad        = body;
	bad.amount = -5.0f;
	bad.speed  = {5.0f, 0.0f, 0.0f};
	bad.flags  = 0xFE;
	Check(CleanGlassBody(bad, s) && s.amount == 0.0f && s.speed.x == 0.0f && !s.explosion,
	      "and so are a negative amount, a speed no car reaches and flags nobody defined");
}

// Car glass is the windscreen, and the windscreen is panel 4 of the damage
// word every car's owner already sends (protocol.md 1.11, cardamage.md). No
// round in retail touches it: the only writer is a collision, through
// VehicleDamage's windscreen arm.
void TestTheWindscreenTravels() {
	std::printf("\ncar glass: the windscreen is a panel and travels with them\n");
	Check(VEHPANEL_WINDSCREEN == 4 && VEHPANEL_WINDSCREEN < VEH_DAMAGE_PANELS,
	      "the windscreen is panel 4, inside the seven the wire carries");
	Check(CarNodeForPanel(VEHPANEL_WINDSCREEN) == CAR_WINDSCREEN && CAR_WINDSCREEN == 0x13 &&
	          !PanelIsBumper(VEHPANEL_WINDSCREEN),
	      "applied through SetPanelDamage on node 13h");

	uint32_t panels = 0;
	SetPanelLevel(panels, VEHPANEL_WINDSCREEN, PANEL_STATUS_SMASHED1);
	Check(CleanPanelWord(panels) == panels, "a cracked windscreen survives the receiver's clean");
	Check(DamageGrew(0, 0, panels, 0), "and is news to a car that had none");
	uint32_t   have = 0;
	uint16_t   doors = 0;
	MergeDamage(have, doors, panels, 0);
	SetPanelLevel(panels, VEHPANEL_WINDSCREEN, PANEL_STATUS_MISSING);
	MergeDamage(have, doors, panels, 0);
	Check(GetPanelLevel(have, VEHPANEL_WINDSCREEN) == PANEL_STATUS_MISSING,
	      "cracked then gone merges to gone");
	Check((LightWordFromPanels(have) & (1u << (VEHPANEL_WINDSCREEN * 2))) != 0,
	      "and the light the engine pairs with it is derived on arrival");
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

// The eight `cmp ax,word ptr [5F5ADCh + 4i]` of an IsGlass chain, wherever
// the compiler put them in the `len` bytes at `from`.
bool IsGlassChainIn(const std::vector<uint8_t> &img, uint32_t from, uint32_t len) {
	int found = 0;
	for (size_t i = 0; i < object::MI_GLASS_COUNT; ++i) {
		const uint32_t want =
		    static_cast<uint32_t>(object::MI_GLASS_FIRST + i * object::MI_GLASS_STRIDE);
		for (uint32_t va = from; va + 7 <= from + len; ++va)
			if (Bytes(img, va, {0x66, 0x3B, 0x05}) && Dword(img, va + 3) == want) {
				++found;
				break;
			}
	}
	return found == static_cast<int>(object::MI_GLASS_COUNT);
}

void TestAgainstTheImage() {
	std::printf("\nglass against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the glass "
		            "against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	const uint32_t shatter = static_cast<uint32_t>(object::CGlass__WindowRespondsToCollision);
	Check(Bytes(img, shatter, {0x53, 0x56, 0x55, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x00503F20, {0x8D, 0xB5, 0x75, 0x01, 0x00, 0x00}) &&
	          Bytes(img, 0x00503F2D, {0x8A, 0x06, 0xC0, 0xE8, 0x04, 0x24, 0x01, 0x74}),
	      "WindowRespondsToCollision opens by returning on bGlassBroken, +0x175 bit 4");
	Check(Bytes(img, 0x00503F40, {0x8A, 0x06, 0x24, 0xF7, 0x0C, 0x08, 0x88, 0x06}) &&
	          Bytes(img, 0x00504617, {0x8A, 0x06, 0x24, 0xEF, 0x0C, 0x10, 0x88, 0x06}),
	      "cracks it first and breaks it last, bits 3 and 4");
	Check(Bytes(img, 0x0050461F, {0xC7, 0x45, 0x3C, 0x00, 0x00, 0xC8, 0xC2}) &&
	          offs::POSITION + 8 == 0x3C && object::GLASS_GONE_Z == -100.0f,
	      "and sends the window's own matrix to z = -100, leaving m_objectMatrix");

	Check(CallsAt(img, object::GLASS_COLLISION_PED_CALL, shatter) &&
	          CallsAt(img, object::GLASS_COLLISION_CALL, shatter) &&
	          CallsAt(img, object::GLASS_BULLET_BREAK_CALL, shatter) &&
	          CallsAt(img, object::GLASS_BLAST_BREAK_CALL, shatter),
	      "the four sites call it");
	int calls = 0;
	for (uint32_t va = 0x00401000; va + 5 <= 0x005E3000; ++va)
		if (CallsAt(img, va, shatter))
			++calls;
	Check(calls == 4, "and nothing else in the image does");
	Check(Bytes(img, 0x00497621, {0x83, 0xC4, 0x24}) &&
	          Bytes(img, 0x00497C4F, {0x83, 0xC4, 0x24}) &&
	          Bytes(img, 0x0050477E, {0x83, 0xC4, 0x24}) &&
	          Bytes(img, 0x00504842, {0x83, 0xC4, 0x24}),
	      "each one cleans nine dwords after the call, so it is __cdecl");
	Check(Bytes(img, 0x004975FF, {0x6A, 0x00}) && Bytes(img, 0x00497C2D, {0x6A, 0x00}) &&
	          Bytes(img, 0x00504758, {0x6A, 0x00}) && Bytes(img, 0x0050481E, {0x6A, 0x01}),
	      "and only the blast passes explosion = 1");
	Check(Bytes(img, 0x00504730, {0xE8}) &&
	          0x00504735 + Dword(img, 0x00504731) == 0x005A41D0 &&
	          Bytes(img, 0x00504738, {0x83, 0xE0, 0x03, 0x83, 0xF8, 0x02, 0x75}),
	      "the round's site sits behind GetRandomNumber() & 3 == 2");
	Check(Bytes(img, 0x0050470C, {0x8A, 0x03, 0xC0, 0xE8, 0x03, 0x24, 0x01, 0x75}),
	      "which only a window already cracked reaches");

	const uint32_t round = static_cast<uint32_t>(object::CGlass__WasGlassHitByBullet);
	Check(Bytes(img, round, {0x53, 0x55, 0x83, 0xEC, 0x10, 0x8B, 0x6C, 0x24, 0x1C}) &&
	          IsGlassChainIn(img, round, 0x90),
	      "WasGlassHitByBullet tests the entity against the eight glass ids");
	Check(IsGlassChainIn(img, 0x0049756A, 0xA0) && IsGlassChainIn(img, 0x00497B98, 0xA0),
	      "so do both collision arms, before their shatter call");
	Check(GLASS_HIT_BY_BULLET_LEAD == object::CGlass__WasGlassHitByBullet,
	      "the drive-by's lead is that function, and these bytes are what settle it");
	Check(CallsAt(img, 0x0055F9A4, round) && CallsAt(img, 0x00560EEB, round) &&
	          CallsAt(img, 0x00562D43, round) && CallsAt(img, 0x00558C2A, round),
	      "and every bullet in the game goes through it");
	for (size_t i = 0; i < object::MI_GLASS_COUNT; ++i)
		if (Dword(img, static_cast<uint32_t>(object::MI_GLASS_FIRST +
		                                     i * object::MI_GLASS_STRIDE)) != 0x0000FFFFu) {
			Check(false, "the glass ids are int16 -1 until load, one every four bytes");
			break;
		} else if (i + 1 == object::MI_GLASS_COUNT) {
			Check(true, "the glass ids are int16 -1 until load, one every four bytes");
		}

	Check(Bytes(img, static_cast<uint32_t>(object::CGlass__WindowRespondsToExplosion),
	            {0x83, 0xEC, 0x18}) &&
	          CallsAt(img, 0x004B1536,
	                  static_cast<uint32_t>(object::CGlass__WindowRespondsToExplosion)) &&
	          Dword(img, 0x005FD904) == 0x41200000u && Dword(img, 0x005FD90C) == 0x461C4000u,
	      "a blast nearer than 10 m shatters with an amount of 10000");

	Check(Bytes(img, 0x0052FCDD, {0x6A, 0x00, 0x6A, 0x04, 0x6A, 0x13}) &&
	          CallsAt(img, 0x0052FCE3, static_cast<uint32_t>(CAutomobile__SetPanelDamage)),
	      "a car's windscreen is panel 4 on node 13h in VehicleDamage, as the wire applies it");
}

} // namespace

int RunGlassTests() {
	g_glassFailures = 0;
	TestWhoShattersAndWhoSays();
	TestTheLatch();
	TestTheWire();
	TestTheWindscreenTravels();
	TestAgainstTheImage();
	return g_glassFailures;
}
