// A mission's change of clothes on every participant: client/src/game/outfit.h
// and UNDRESS_CHAR's place on the replay list (game/replay.h).
//
// Nothing here runs a script. What can be checked is what the owner sends,
// the participant's decision frame by frame, and - with a retail exe handed
// over - the two handlers the participant runs.

#include "game/outfit.h"
#include "game/replay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_outfitFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_outfitFailures;
}

OutfitInputs OnFoot(const char *model0, bool loaded = true) {
	OutfitInputs in;
	in.havePed      = true;
	in.inVehicle    = false;
	in.pedState     = 1;   // PED_IDLE
	in.model0       = model0;
	in.model0Loaded = loaded;
	return in;
}

void TestWhatTheOwnerSends() {
	std::printf("the owner's UNDRESS_CHAR on the wire\n");
	using namespace replay;
	const Entry *e = Find(0x0352);
	Check(e && e->kind == Kind::Outfit && e->count == 2 && e->args[0] == Arg::Value &&
	          e->args[1] == Arg::Text,
	      "UNDRESS_CHAR is on the list as the player's clothes: a char, then a label");
	Check(!Listed(0x0353), "DRESS_CHAR is not: each machine dresses its own when its model is in");

	// 0352: undress $PLAYER_ACTOR ($ at 0x40) into 'PLAYER'.
	std::vector<uint8_t> space(0x400, 0);
	const int32_t        handle = 0x1234;
	std::memcpy(space.data() + 0x40, &handle, 4);
	const uint8_t operands[] = {0x02, 0x40, 0x00, 'P', 'L', 'A', 'Y', 'E', 'R', 0, 0};
	std::memcpy(space.data() + 0x100, operands, sizeof operands);
	Encoded enc;
	Check(Encode(0x0352, space.data(), 0x400, 0x100, nullptr, &enc) && enc.kind == Kind::Outfit &&
	          enc.length == 2 + 5 + 8,
	      "it encodes as the char's value and the eight bytes of label");
	int32_t v = 0;
	Check(LiteralAt(enc.code, enc.length, 0, &v) && v == handle,
	      "the owner's handle is read off the global");
	Check(SetLiteralAt(enc.code, enc.length, 0, 0) && LiteralAt(enc.code, enc.length, 0, &v) &&
	          v == 0,
	      "and can be blanked before it goes, since it means nothing anywhere else");
	bool anyHandle = false;
	uint8_t copy[MAX_CODE];
	std::memcpy(copy, enc.code, enc.length);
	EachHandle(copy, enc.length, [&](Arg, int32_t *) {
		anyHandle = true;
		return true;
	});
	Check(!anyHandle, "no operand of it is a pedestrian the session has to name first");
	char look[PLAYER_LOOK_LEN];
	Check(LookFromLabel(enc.code + 7, look) && std::strcmp(look, "player") == 0,
	      "and its label reads back as the look, lowered as the engine lowers it");
}

void TestTheLabel() {
	std::printf("a look and the label that carries it\n");
	uint8_t label[8];
	char    look[PLAYER_LOOK_LEN];
	Check(OutfitLabel("playerp", label) && label[6] == 'p' && label[7] == 0 &&
	          LookFromLabel(label, look) && std::strcmp(look, "playerp") == 0,
	      "'playerp' goes into eight bytes and comes back");
	const uint8_t full[8] = {'P', 'L', 'A', 'Y', 'E', 'R', 'X', 'Y'};
	Check(LookFromLabel(full, look) && std::strcmp(look, "playerxy") == 0,
	      "a label with no terminator is all eight");
	Check(!OutfitLabel("player123", label), "a name longer than a label is refused");
	const uint8_t junk[8] = {'p', 'l', 'a', '!', 0, 0, 0, 0};
	Check(!LookFromLabel(junk, look), "and so is one the look sync would not carry");
}

void TestTheParticipantsTurn() {
	std::printf("a participant follows the mission's change\n");
	OutfitChange o;
	Check(!o.Want("eight2", "playerp") && !o.Pending(),
	      "a look that isn't Claude's is nobody's to put on");
	Check(!o.Want("PLAYERP", "playerp") && !o.Pending(), "nor one we already wear");
	Check(o.Want("PLAYER", "playerp") && o.Pending() && std::strcmp(o.Look(), "player") == 0,
	      "the prison clothes going back to 'player' is wanted");

	OutfitInputs in = OnFoot("playerp");
	in.inVehicle    = true;
	in.pedState     = PEDSTATE_DRIVING;
	Check(o.Next(in) == OutfitStep::Nothing && o.Pending(),
	      "not in a seat no car of ours is known for, where nothing could sit it back down");
	in = OnFoot("playerp");
	for (uint32_t s : {PEDSTATE_ENTER_CAR, PEDSTATE_EXIT_CAR, PEDSTATE_CARJACK,
	                   PEDSTATE_DRAG_FROM_CAR, PEDSTATE_DIE, PEDSTATE_DEAD, PEDSTATE_ARRESTED}) {
		in.pedState = s;
		if (o.Next(in) != OutfitStep::Nothing) {
			Check(false, "a door, a death or an arrest waits");
			break;
		}
	}
	Check(o.Pending(), "a door, a death or an arrest waits, and the change is still owed");
	in          = OnFoot("playerp");
	in.havePed  = false;
	Check(o.Next(in) == OutfitStep::Nothing && o.Pending(), "and so does having no ped at all");

	Check(o.Next(OnFoot("playerp")) == OutfitStep::Undress, "on foot, it is taken apart");
	o.Undressed();
	Check(o.Next(OnFoot("player", false)) == OutfitStep::Nothing && o.Pending(),
	      "and waits, out of the world, for the model");
	Check(o.Next(OnFoot("player", true)) == OutfitStep::Dress, "then it is built again");
	o.Dressed();
	Check(!o.Pending() && o.Next(OnFoot("player")) == OutfitStep::Nothing, "once");
}

OutfitInputs InCar(const char *model0, bool driver, int32_t type = VEHICLE_TYPE_CAR) {
	OutfitInputs in = OnFoot(model0);
	in.inVehicle    = true;
	in.pedState     = PEDSTATE_DRIVING;
	in.haveCar      = true;
	in.carType      = type;
	in.driver       = driver;
	in.passenger    = !driver;
	return in;
}

void TestInACarSeat() {
	std::printf("a participant riding in a car when the mission changes\n");
	// The run of 2026-09-25: the order came at 121 s with our player in the
	// back of the owner's car, and it stayed there through Luigi's scene.
	OutfitChange o;
	Check(o.Want("player", "playerp"), "wanted");
	Check(o.Next(InCar("playerp", false)) == OutfitStep::Undress,
	      "a passenger is taken apart where they sit, not when they get out");
	o.Undressed();
	Check(o.Next(InCar("player", false)) == OutfitStep::Dress && (o.Dressed(), !o.Pending()),
	      "and built again in the seat");

	Check(o.Want("player", "playerp") && o.Next(InCar("playerp", true)) == OutfitStep::Undress,
	      "so is the driver");
	o.Clear();

	Check(o.Want("player", "playerp"), "wanted again");
	OutfitInputs in = InCar("playerp", false);
	in.passenger    = false;
	Check(o.Next(in) == OutfitStep::Nothing && o.Pending(),
	      "not in a car whose seats don't have us in them");
	in         = InCar("playerp", false);
	in.haveCar = false;
	Check(o.Next(in) == OutfitStep::Nothing && o.Pending(), "nor with no car at all");
	Check(o.Next(InCar("playerp", true, VEHICLE_TYPE_BOAT)) == OutfitStep::Nothing && o.Pending(),
	      "nor at a boat's wheel, which PedSetInCarCB sits in no animation");
	Check(o.Next(InCar("playerp", false, VEHICLE_TYPE_TRAIN)) == OutfitStep::Nothing && o.Pending(),
	      "nor on a train, which seats nobody through it");
	for (uint32_t s : {PEDSTATE_ENTER_CAR, PEDSTATE_EXIT_CAR, PEDSTATE_CARJACK, PEDSTATE_DRAG_FROM_CAR,
	                   PEDSTATE_DIE, PEDSTATE_DEAD}) {
		in          = InCar("playerp", true);
		in.pedState = s;
		if (o.Next(in) != OutfitStep::Nothing) {
			Check(false, "a door, a jack or a death in the car still waits");
			break;
		}
	}
	Check(o.Pending(), "a door, a jack or a death in the car still waits");
	in           = InCar("playerp", false);
	in.inVehicle = false;
	Check(o.Next(in) == OutfitStep::Nothing && o.Pending(),
	      "and a seat that's still on its way in, bInVehicle not yet set, waits");
	Check(o.Next(InCar("playerp", false)) == OutfitStep::Undress, "the seat once it's sat in");

	std::printf("the pose a rebuilt seat is sat in\n");
	OutfitInputs seat = InCar("player", true);
	Check(OutfitSeated(seat) && OutfitSitAnim(seat) == ANIM_STD_CAR_SIT, "the driver sits CAR_SIT");
	seat.lowCar = true;
	Check(OutfitSitAnim(seat) == ANIM_STD_CAR_SIT_LO, "in a low car CAR_LSIT");
	seat = InCar("player", false);
	Check(OutfitSitAnim(seat) == ANIM_STD_CAR_SIT_P, "a passenger CAR_SITP");
	seat.lowCar = true;
	Check(OutfitSitAnim(seat) == ANIM_STD_CAR_SIT_P_LO, "in a low car CAR_SITPLO");
	Check(!OutfitSeated(OnFoot("player")), "and on foot there is no seat to sit back down in");
}

void TestWhatIsNotOursToChange() {
	std::printf("what the change leaves alone\n");
	OutfitChange o;
	Check(o.Want("player", "playerp"), "wanted");
	Check(o.Next(OnFoot("playerx")) == OutfitStep::Nothing && !o.Pending(),
	      "our own script dressing us in something else first ends it");

	Check(o.Want("player", "playerp"), "wanted again");
	Check(o.Next(OnFoot("player")) == OutfitStep::Nothing && !o.Pending(),
	      "being in it already ends it too");

	Check(o.Want("player", "playerp") && o.Next(OnFoot("playerp")) == OutfitStep::Undress,
	      "taken apart");
	o.Undressed();
	OutfitInputs gone = OnFoot("player");
	gone.havePed      = false;
	Check(o.Next(gone) == OutfitStep::Nothing && !o.Pending(),
	      "a ped gone before it was dressed leaves nothing to dress");

	Check(o.Want("player", "playerp") && o.Next(OnFoot("playerp", false)) == OutfitStep::Nothing,
	      "nor is a model 0 somebody else is still loading taken apart under them");
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
	std::printf("\nUNDRESS_CHAR and DRESS_CHAR against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "clothes against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	constexpr uint32_t UNDRESS = 0x0044AAFC, DRESS = 0x0044ABBA;
	Check(Dword(img, g_ScriptOpcodeTable_800 + (0x0352 - 800) * 4) == UNDRESS &&
	          Dword(img, g_ScriptOpcodeTable_800 + (0x0353 - 800) * 4) == DRESS,
	      "0352 and 0353 are the 800 table's entries 50 and 51");
	Check(CallsAt(img, 0x0044AB0A, CTheScripts__CollectParameters) &&
	          Bytes(img, 0x0044AB07, {0x6A, 0x01}),
	      "UNDRESS_CHAR collects one operand, the char");
	Check(Bytes(img, 0x0044AB35, {0x6A, 0x08}) && Bytes(img, 0x0044AB95, {0x83, 0x40, 0x10, 0x08}),
	      "then takes eight bytes of label with no type byte, and steps over them");
	Check(Bytes(img, 0x0044AB4D, {0x80, 0xF9, 0x41}) && Bytes(img, 0x0044AB57, {0x80, 0x84, 0x14}),
	      "lowering it as it goes");
	Check(CallsAt(img, 0x0044AB72, CPed__IsPlayer) && Bytes(img, 0x0044AB7B, {0x31, 0xDB}),
	      "for the player it renames model 0");
	Check(Bytes(img, 0x0044AB84, {0x6A, 0x06}) &&
	          CallsAt(img, 0x0044AB88, CStreaming__RequestSpecialModel),
	      "through RequestSpecialModel, the call the look hook sits on");
	Check(CallsAt(img, 0x0044AB99, CWorld__Remove), "and takes the ped out of the world");
	Check(CallsAt(img, 0x0044ABC8, CTheScripts__CollectParameters) &&
	          Bytes(img, 0x0044ABE6, {0x66, 0x83, 0x4E, 0x5C, 0xFF}) &&
	          Bytes(img, 0x0044ABEE, {0xFF, 0x57, 0x0C}) && CallsAt(img, 0x0044ABF2, CWorld__Add),
	      "DRESS_CHAR builds it again from its own model index and puts it back");

	std::printf("\na seated ped built again, against gta3.exe\n");
	// The player's vtable: ~CPlayerPed stamps it and frees a member past the
	// end of a CPed.
	constexpr uint32_t PLAYER_VTABLE = 0x005FA500;
	Check(Bytes(img, 0x004EFB33, {0xC7, 0x03, 0x00, 0xA5, 0x5F, 0x00}) &&
	          Bytes(img, 0x004EFB39, {0x8B, 0x83, 0x3C, 0x05, 0x00, 0x00}),
	      "~CPlayerPed stamps 0x005FA500 and frees [+53Ch], past SIZEOF_PED");
	Check(Dword(img, PLAYER_VTABLE + 6 * 4) == 0x00473F90 && Dword(img, PLAYER_VTABLE + 3 * 4) == 0x004C52A0,
	      "UNDRESS_CHAR's [+18h] and DRESS_CHAR's [+0Ch] on the player are DeleteRwObject and "
	      "CPed::SetModelIndex");
	Check(Bytes(img, 0x004C5301, {0x6A, 0x03}) && Bytes(img, 0x004C52F2, {0x89, 0x83, 0xD4, 0x01, 0x00, 0x00}),
	      "SetModelIndex leaves the new clump in ANIM_STD_IDLE of the model's own group");
	Check(Bytes(img, 0x004CF31E, {0x83, 0xBD, 0x84, 0x02, 0x00, 0x00, 0x01}),
	      "PedSetInCarCB takes boats (m_vehType 1) off on their own arm");
	Check(Bytes(img, 0x004CF7AC, {0x39, 0x9D, 0xA4, 0x01, 0x00, 0x00}) &&
	          Bytes(img, 0x004CF7B4, {0x8A, 0x85, 0xF6, 0x01, 0x00, 0x00, 0xC0, 0xE8, 0x03}),
	      "its sitting animation turns on pDriver and on bLowVehicle, bit 3 of +1F6h");
	Check(Bytes(img, 0x004CF7C7, {0x6A, ANIM_STD_CAR_SIT_LO}) && Bytes(img, 0x004CF7D6, {0x6A, ANIM_STD_CAR_SIT}) &&
	          Bytes(img, 0x004CF7F3, {0x6A, ANIM_STD_CAR_SIT_P_LO}) &&
	          Bytes(img, 0x004CF7FD, {0x6A, ANIM_STD_CAR_SIT_P}),
	      "driver 6Fh/70h low, passenger 71h/72h low");
	Check(Bytes(img, 0x004CF7F7, {0xFF, 0x35, 0x74, 0x84, 0x5F, 0x00}) &&
	          Dword(img, 0x005F8474) == 0x42C80000,
	      "blended in at 100.0f");
	Check(CallsAt(img, 0x004CF805, CAnimManager__BlendAnimation) &&
	          Bytes(img, 0x004CF80D, {0x89, 0x83, 0xD8, 0x01, 0x00, 0x00}) &&
	          CallsAt(img, 0x004CF815, CPed__StopNonPartialAnims),
	      "into m_pVehicleAnim, then StopNonPartialAnims");
	Check(CallsAt(img, 0x0043E7EE, CAnimManager__BlendAnimation) &&
	          Bytes(img, 0x0043E7F6, {0x89, 0x85, 0xD8, 0x01, 0x00, 0x00}) &&
	          CallsAt(img, 0x0043E7FE, CPed__StopNonPartialAnims) &&
	          CallsAt(img, 0x00441B9E, CAnimManager__BlendAnimation) &&
	          CallsAt(img, 0x00441BAE, CPed__StopNonPartialAnims),
	      "which is how CREATE_CHAR_INSIDE_CAR and CREATE_CHAR_AS_PASSENGER seat a ped just built");
	Check(Bytes(img, 0x004DF98F, {0x6A, ANIM_STD_CAR_SIT}) && Bytes(img, 0x004DF9B4, {0x6A, ANIM_STD_CAR_SIT_LO}) &&
	          Bytes(img, 0x004DF9D9, {0x6A, ANIM_STD_CAR_SIT_P}) &&
	          Bytes(img, 0x004DFA03, {0x6A, ANIM_STD_CAR_SIT_P_LO}) &&
	          CallsAt(img, 0x004DF99F, 0x004D4970) && CallsAt(img, 0x004DFA13, 0x004D4970),
	      "and LineUpPedWithCar keeps a ped in its seat only while one of the four is on it");
	Check(Bytes(img, CPed__StopNonPartialAnims, {0x8B, 0x41, 0x4C}) &&
	          Bytes(img, 0x004C5D63, {0x83, 0xE2, 0x10}) && Bytes(img, 0x004C5D68, {0x83, 0x60, 0x30, 0xFE}),
	      "StopNonPartialAnims stops everything of the clump's but the partials");
}

} // namespace

int RunOutfitTests() {
	g_outfitFailures = 0;
	TestWhatTheOwnerSends();
	TestTheLabel();
	TestTheParticipantsTurn();
	TestInACarSeat();
	TestWhatIsNotOursToChange();
	TestAgainstTheImage();
	return g_outfitFailures;
}
