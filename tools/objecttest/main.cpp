// Unit tests for the breakable-street-object seam. No game, no socket.
//
//   xmake build objecttest && xmake run objecttest
//
// Three things are worth testing here and all three are testable without GTA
// III running:
//
// 1. **The identity matcher**, over a pool this file builds by hand out of
//    the same offsets and the same 0x19C stride client/src/game/addresses.h
//    gives the engine. That is the whole feature: get the naming wrong and
//    the wrong crate breaks, or nothing does. The fabricated pool is not a
//    mock of the lookup - it is the real FindObjectByIdent walking real
//    bytes at real offsets.
//
// 2. **The break-state arithmetic**, which is two flag bytes in and two bits
//    out, plus "how many more times does the engine have to run here".
//
// 3. **Who is entitled to report**, as a truth table rather than as a
//    paragraph in a design document.
//
// What is NOT covered, said plainly: nothing here calls into gta3.exe, so the
// detours themselves, the pool pointer, and whether CObject::ObjectDamage
// actually breaks a lamp post when we replay it have not been exercised.
// docs/objects.md 9 lists those.

#include "client.h"
#include "game/addresses.h"
#include "game/object.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace obj = coopiii::game::object;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

ObjectIdent Ident(float x, float y, float z, int16_t model) {
	ObjectIdent id{};
	id.pos.x      = x;
	id.pos.y      = y;
	id.pos.z      = z;
	id.modelIndex = model;
	return id;
}

// ---------------------------------------------------------------------------
// A fabricated object pool
// ---------------------------------------------------------------------------
//
// Laid out exactly as CPool does it: one entry array of `stride` bytes per
// slot, one parallel byte of flags with POOLFLAG_ISFREE in bit 7. Every field
// below is written at the offset addresses.h says the engine reads it from,
// so a mistake in either place shows up here rather than in the game.
class FakePool {
public:
	explicit FakePool(int32_t slots)
	    : m_entries(static_cast<size_t>(slots) * obj::OBJECT_POOL_STRIDE, 0),
	      m_flags(static_cast<size_t>(slots), obj::POOLFLAG_ISFREE),
	      m_size(slots) {}

	// A live map object at a placement, as CObject::CObject(CDummyObject*)
	// would leave it: the IPL coordinate in m_objectMatrix, the model index
	// in CEntity, ObjectCreatedBy == GAME_OBJECT.
	uint8_t *Place(int32_t slot, float x, float y, float z, int16_t model,
	               uint8_t createdBy = obj::GAME_OBJECT,
	               bool isPickup = false) {
		uint8_t *e = Slot(slot);
		m_flags[static_cast<size_t>(slot)] = 0;   // not free

		auto *matrixPos = reinterpret_cast<float *>(e + obj::OBJECT_MATRIX_POS);
		matrixPos[0] = x;
		matrixPos[1] = y;
		matrixPos[2] = z;

		// The live position too, set somewhere else on purpose: the matcher
		// must read m_objectMatrix and never this.
		auto *livePos = reinterpret_cast<float *>(e + offs::POSITION);
		livePos[0] = x + 500.0f;
		livePos[1] = y - 500.0f;
		livePos[2] = z + 50.0f;

		*reinterpret_cast<int16_t *>(e + offs::MODEL_INDEX) = model;
		e[obj::CREATED_BY] = createdBy;
		if (isPickup)
			e[obj::OBJECT_FLAGS] |= obj::OBJ_IS_PICKUP;
		return e;
	}

	void Free(int32_t slot) {
		m_flags[static_cast<size_t>(slot)] = obj::POOLFLAG_ISFREE;
	}

	uint8_t *Slot(int32_t slot) {
		return m_entries.data() +
		       static_cast<size_t>(slot) * obj::OBJECT_POOL_STRIDE;
	}

	ObjectPoolView View() {
		ObjectPoolView v;
		v.entries = m_entries.data();
		v.flags   = m_flags.data();
		v.size    = m_size;
		v.stride  = obj::OBJECT_POOL_STRIDE;
		return v;
	}

private:
	std::vector<uint8_t> m_entries;
	std::vector<uint8_t> m_flags;
	int32_t              m_size;
};

// ---------------------------------------------------------------------------

void TestWireLayout() {
	std::printf("wire layout\n");
	Check(sizeof(ObjectIdent) == 16, "ObjectIdent is 16 bytes");
	Check(offsetof(ObjectIdent, modelIndex) == 12, "the model follows the position");
	Check(sizeof(ObjectBreakBody) == 24, "ObjectBreakBody is 24 bytes");
	Check(offsetof(ObjectBreakBody, amount) == 16, "the amount follows the ident");
	Check(offsetof(ObjectBreakBody, state) == 20, "the state follows the amount");
	Check(sizeof(C_ObjectBroken) == 29, "C_ObjectBroken is 29 bytes");
	Check(sizeof(S_ObjectBroken) == 30, "S_ObjectBroken is a byte more, for the reporter");
	Check(C_ObjectBroken::OPCODE == 0xC0 && S_ObjectBroken::OPCODE == 0xC1,
	      "the opcodes are the reserved 0xC0 pair");

	Check(sizeof(ObjectRestBody) == 64, "ObjectRestBody is 64 bytes");
	Check(offsetof(ObjectRestBody, right) == 16, "the matrix follows the ident");
	Check(offsetof(ObjectRestBody, forward) == 28 &&
	          offsetof(ObjectRestBody, up) == 40 &&
	          offsetof(ObjectRestBody, pos) == 52,
	      "right, forward, up, position - CMatrix's own order");
	Check(sizeof(C_ObjectSettled) == 69, "C_ObjectSettled is 69 bytes");
	Check(sizeof(S_ObjectSettled) == 70, "S_ObjectSettled is a byte more, for the reporter");
	Check(C_ObjectSettled::OPCODE == 0xC2 && S_ObjectSettled::OPCODE == 0xC3,
	      "and the resting place uses the next reserved pair");

	// The three bits of the state byte are distinct and the top five are
	// still free. Worth pinning because the uproot bit was added to a byte
	// that already had two meanings and a collision between them would look
	// like "sometimes the crate does not burst".
	Check(OBJ_BREAK_RENDER_DAMAGED == 1 && OBJ_BREAK_SMASHED == 2 &&
	          OBJ_BREAK_UPROOTED == 4,
	      "the three state bits do not overlap");
}

// ---------------------------------------------------------------------------
// What object.dat says, as a regression test rather than as a paragraph
// ---------------------------------------------------------------------------
//
// The brief this work started from said a lamp post that is *shot* falls over
// on the shooter's screen only. It does not fall over on anybody's screen,
// and the reason is one column of a text file that ships with the game.
//
// CWeapon::DoBulletImpact, CWeapon::FireShotgun and CWeapon::FireMelee share
// one object arm, and its uproot gate is `GetIsStatic() && m_fUprootLimit <=
// 0.0f` - verified against the retail image, including that the three
// constants compared against (0x00603060, 0x00603060, 0x00602C88) all read
// 00000000, so it really is `<= 0` and not a threshold. The move force that
// follows is behind `!GetIsStatic()`, so a static object gets neither.
//
// Column G of data/object.dat is the number that gate reads, and for every
// piece of street furniture worth shooting it is well above zero.
void TestABulletCannotUprootALampPost() {
	std::printf("what a bullet can and cannot knock over\n");

	// data/object.dat, column G, for the breakable models that appear in the
	// map in any number. These are data, not measurements - the file ships
	// with the game and is identical on every install, which is the same
	// property the whole identity scheme rests on.
	struct Furniture { const char *model; float uprootLimit; int instances; };
	constexpr Furniture kBreakables[] = {
	    {"doublestreetlght1", 400.0f, 392},
	    {"trafficlight1",     500.0f, 333},
	    {"lamppost3",         400.0f, 184},
	    {"lamppost1",         400.0f, 119},
	    {"bar_barrier10",     350.0f, 107},
	    {"parkbench1",          5.0f,  70},
	    {"lamppost2",         400.0f,  67},
	    {"trafficcone",        10.0f,  39},
	    {"parkingmeter",      100.0f,   0},
	    {"bin1",              100.0f,   0},
	    {"postbox1",          100.0f,   0},
	    {"fire_hydrant",      100.0f,   0},
	    {"smashbar",         1000.0f,   0},
	};

	bool anyUprootable = false;
	for (const Furniture &f : kBreakables)
		if (f.uprootLimit <= 0.0f)
			anyUprootable = true;
	Check(!anyUprootable,
	      "no lamp post, traffic light, meter, bin, bench, cone or barrier has "
	      "an uproot limit a bullet can beat");

	// The ones it can, and they are all boxes. This half matters as much as
	// the other: a player shooting a stack of crates really does knock them
	// loose, on every machine, because docs/protocol.md 1.9.2 replays the
	// shot through the engine and every observer's own DoBulletImpact runs
	// the same arm.
	constexpr Furniture kLoose[] = {
	    {"cardboardbox4", 0.0f, 95},
	    {"woodenbox",     0.0f, 64},
	    {"cardboardbox2", 0.0f, 25},
	    {"papermachn01",  0.0f, 20},
	    {"wastebin",      0.0f,  0},
	    {"palette",       0.0f,  0},
	};
	bool allLoose = true;
	for (const Furniture &f : kLoose)
		if (f.uprootLimit > 0.0f)
			allLoose = false;
	Check(allLoose, "the boxes and bins a bullet does move are the zero-limit ones");

	// And the threshold a break goes through is a different number from the
	// one an uproot goes through, which is why they are two decisions and not
	// one. A lamp post bends at 150 impulse and comes down at 400.
	Check(obj::OBJECT_DAMAGE_THRESHOLD == 150.0f,
	      "a break is amount * multiplier > 150");
	Check(obj::OBJECT_DAMAGE_THRESHOLD < 400.0f,
	      "and a lamp post's uproot limit is above it, so it can bend without "
	      "coming down");
}

void TestSameObject() {
	std::printf("identity\n");
	const ObjectIdent a = Ident(100.0f, -200.0f, 15.0f, 1393);

	Check(SameObject(a, a), "an ident names itself");
	Check(SameObject(a, Ident(100.2f, -200.0f, 15.0f, 1393)),
	      "0.2 m away is the same lamp post");
	Check(!SameObject(a, Ident(100.3f, -200.0f, 15.0f, 1393)),
	      "0.3 m away is not");
	Check(!SameObject(a, Ident(100.0f, -200.0f, 15.0f, 1404)),
	      "the same spot with a different model is a different object");

	// The measurement docs/objects.md 4 records, as a regression test rather
	// than as a sentence. The closest same-model pair among all 1851
	// breakable map instances is two papermachn01 at 0.5992 m; the tolerance
	// has to stay under that or a query aimed at one of them could land on
	// the other. Half of it is the margin this was chosen with.
	constexpr float kClosestSameModelPairInLibertyCity = 0.5992f;
	Check(kObjectIdentTolerance < kClosestSameModelPairInLibertyCity,
	      "the tolerance cannot reach the closest same-model pair in the map");
	Check(kObjectIdentTolerance <= kClosestSameModelPairInLibertyCity / 2.0f,
	      "and it keeps a factor of two in hand");
}

void TestBreakState() {
	std::printf("break state\n");
	// bUsesCollision is bit 0 of byte A; bIsVisible is bit 2 and
	// bRenderDamaged is bit 7 of byte B. A pristine object uses collision and
	// is visible.
	constexpr uint8_t kUsesCollision = 0x01;
	constexpr uint8_t kIsVisible     = 0x04;
	constexpr uint8_t kRenderDamaged = 0x80;

	Check(BreakStateFromFlags(kUsesCollision, kIsVisible) == 0,
	      "a pristine object is not broken");
	Check(BreakStateFromFlags(kUsesCollision, kIsVisible | kRenderDamaged) ==
	          OBJ_BREAK_RENDER_DAMAGED,
	      "bRenderDamaged alone is the damaged-model state");
	Check(BreakStateFromFlags(0, 0) == OBJ_BREAK_SMASHED,
	      "invisible and non-colliding is smashed");
	Check(BreakStateFromFlags(0, kRenderDamaged) ==
	          (OBJ_BREAK_SMASHED | OBJ_BREAK_RENDER_DAMAGED),
	      "change-then-smash ends up as both");

	// The two halves of "smashed" mean nothing on their own, and the engine
	// really does produce each of them without anything being broken: a glass
	// pane is created with bIsVisible already false
	// (CPopulation::ConvertToRealObject), and CPed::WarpPedIntoCar clears
	// bUsesCollision on a perfectly healthy ped.
	Check(BreakStateFromFlags(kUsesCollision, 0) == 0,
	      "invisible on its own is not broken");
	Check(BreakStateFromFlags(0, kIsVisible) == 0,
	      "no collision on its own is not broken");

	// bIsStatic is byte A bit 2, and clearing it is the whole of "uprooted".
	// It is deliberately not part of BreakStateFromFlags: a smash *sets*
	// bIsStatic, so folding the two together would make finishing an object
	// off look like standing it back up.
	constexpr uint8_t kIsStatic = 0x04;
	Check(!ObjectIsLoose(kIsStatic), "a static object is standing");
	Check(ObjectIsLoose(kUsesCollision), "one with bIsStatic clear has come loose");

	Check(BreakStateWithUproot(kUsesCollision | kIsStatic, kIsVisible) == 0,
	      "a pristine standing post is neither broken nor loose");
	Check(BreakStateWithUproot(kUsesCollision, kIsVisible) == OBJ_BREAK_UPROOTED,
	      "a pristine post that came loose says only that");
	Check(BreakStateWithUproot(kUsesCollision, kIsVisible | kRenderDamaged) ==
	          (OBJ_BREAK_RENDER_DAMAGED | OBJ_BREAK_UPROOTED),
	      "and the car that bends a lamp post usually knocks it down too");
	Check(BreakStateWithUproot(kIsStatic, 0) == OBJ_BREAK_SMASHED,
	      "a smash re-statics the object, so a smashed crate is not 'loose'");
}

void TestReplayCount() {
	std::printf("how many replays\n");
	Check(BreakReplaysNeeded(0, 0) == 0, "nothing to do");
	Check(BreakReplaysNeeded(0, OBJ_BREAK_RENDER_DAMAGED) == 1,
	      "pristine to damaged-model is one hit");
	Check(BreakReplaysNeeded(0, OBJ_BREAK_SMASHED) == 2,
	      "pristine to smashed is two, which is what change-then-smash needs");
	Check(BreakReplaysNeeded(OBJ_BREAK_RENDER_DAMAGED, OBJ_BREAK_SMASHED) == 1,
	      "already bent, so one more finishes it");
	Check(BreakReplaysNeeded(OBJ_BREAK_RENDER_DAMAGED,
	                         OBJ_BREAK_RENDER_DAMAGED) == 0,
	      "already there");
	Check(BreakReplaysNeeded(OBJ_BREAK_SMASHED, OBJ_BREAK_RENDER_DAMAGED) == 0,
	      "a copy that is already smashed is never walked backwards");
	Check(BreakReplaysNeeded(OBJ_BREAK_SMASHED, 0) == 0,
	      "and a late packet about a pristine object does not undo it");

	// The uproot bit is not a break and must not change the count. Nothing
	// ObjectDamage does can produce it, so a replay aimed at it would run
	// forever - or, worse, once too often on a change-then-smash object.
	Check(BreakReplaysNeeded(0, OBJ_BREAK_UPROOTED) == 0,
	      "a post that only came loose needs no ObjectDamage at all");
	Check(BreakReplaysNeeded(0, OBJ_BREAK_RENDER_DAMAGED | OBJ_BREAK_UPROOTED) == 1,
	      "bent and loose is still one hit");
	Check(BreakReplaysNeeded(OBJ_BREAK_UPROOTED,
	                         OBJ_BREAK_SMASHED | OBJ_BREAK_UPROOTED) == 2,
	      "and a loose local copy is not treated as already half broken");
}

void TestWhoSaysWhereItLanded() {
	std::printf("who follows an object that came loose\n");

	// A collision wrote m_fDamageImpulse and m_pDamageEntity this frame, so
	// the ordinary ownership answer stands unchanged.
	Check(UprootCause(/*hadImpulse*/ true, BreakCause::OURS, false) ==
	          BreakCause::OURS,
	      "our own car knocking a post down is ours to follow");
	Check(UprootCause(true, BreakCause::REPLICA, false) == BreakCause::REPLICA,
	      "a replica's collision is its owner's");
	Check(UprootCause(true, BreakCause::NOBODY, false) == BreakCause::NOBODY,
	      "and an ownerless collision stays ownerless");

	// No impulse means nothing collided with it, and the only other thing in
	// the image that clears bIsStatic on a breakable map object is the object
	// arm shared by DoBulletImpact, FireShotgun and FireMelee - which only
	// ever runs inside somebody's CWeapon::Fire. So the shooter knows it was
	// theirs because the engine is still inside their own trigger pull.
	Check(UprootCause(false, BreakCause::NOBODY, /*replaying*/ false) ==
	          BreakCause::OURS,
	      "a bullet with nobody replaying a shot is our own trigger pull");
	Check(UprootCause(false, BreakCause::NOBODY, /*replaying*/ true) ==
	          BreakCause::REPLICA,
	      "and one fired inside a replay is the shooter's to follow, not ours");

	// The whole point of the arm above: a non-host shooting a crate reports
	// it. Under the old rule this was NOBODY, which MayReportBreak hands to
	// the host - the one machine in the session that might be 80 m away and
	// hold no CObject at all.
	Check(MayReportBreak(obj::GAME_OBJECT, false, true, false,
	                     UprootCause(false, BreakCause::NOBODY, false),
	                     /*isHost*/ false),
	      "a non-host who shoots a crate loose is the one who reports it");
	Check(!MayReportBreak(obj::GAME_OBJECT, false, true, false,
	                      UprootCause(false, BreakCause::NOBODY, true),
	                      /*isHost*/ true),
	      "and the host stays quiet about somebody else's replayed shot");
}

void TestARestingPlaceOffTheWire() {
	std::printf("a matrix we are willing to write into an entity\n");

	ObjectRestBody good{};
	good.ident   = Ident(100.0f, -200.0f, 15.0f, 1393);
	good.right   = {1.0f, 0.0f, 0.0f};
	good.forward = {0.0f, 1.0f, 0.0f};
	good.up      = {0.0f, 0.0f, 1.0f};
	good.pos     = {100.0f, -200.0f, 14.5f};
	Check(SaneRotation(good), "an identity rotation is fine");

	// A post lying down is the case this whole packet exists for, and its
	// rows are still unit length - they are just pointing somewhere else.
	ObjectRestBody fallen = good;
	fallen.forward = {0.0f, 0.0f, -1.0f};
	fallen.up      = {0.0f, 1.0f, 0.0f};
	Check(SaneRotation(fallen), "and so is a post lying on its side");

	ObjectRestBody zeroed{};
	Check(!SaneRotation(zeroed),
	      "an all-zero body is refused - that is a truncated or forged packet, "
	      "and writing it would collapse the object's collision to a point");

	ObjectRestBody nan = good;
	nan.up.z = std::nanf("");
	Check(!SaneRotation(nan), "a NaN in the matrix is refused before it propagates");

	ObjectRestBody huge = good;
	huge.pos.x = 1.0e30f;
	Check(!SaneRotation(huge), "and so is a position no sector index could hold");

	ObjectRestBody stretched = good;
	stretched.right = {10.0f, 0.0f, 0.0f};
	Check(!SaneRotation(stretched), "a row ten times unit length is not a rotation");
}

void TestWhoReports() {
	std::printf("who reports\n");
	constexpr bool kHost    = true;
	constexpr bool kNotHost = false;
	constexpr bool kSession = true;
	constexpr bool kBlast   = true;
	constexpr bool kNoBlast = false;
	constexpr bool kPickup  = true;
	constexpr bool kNotPickup = false;

	Check(MayReportBreak(obj::GAME_OBJECT, kNotPickup, kSession, kNoBlast,
	                     BreakCause::OURS, kNotHost),
	      "our own car knocking down a post is ours to report");
	Check(!MayReportBreak(obj::GAME_OBJECT, kNotPickup, kSession, kNoBlast,
	                      BreakCause::REPLICA, kHost),
	      "a replica's collision is the owner's to report, even on the host");
	Check(MayReportBreak(obj::GAME_OBJECT, kNotPickup, kSession, kNoBlast,
	                     BreakCause::NOBODY, kHost),
	      "nobody's break is the host's, as roadmap 5.8 settled for a car");
	Check(!MayReportBreak(obj::GAME_OBJECT, kNotPickup, kSession, kNoBlast,
	                      BreakCause::NOBODY, kNotHost),
	      "and a non-host stays quiet about it");

	Check(!MayReportBreak(obj::GAME_OBJECT, kNotPickup, kSession, kBlast,
	                      BreakCause::OURS, kHost),
	      "an explosion says nothing: every machine already agreed");
	Check(!MayReportBreak(obj::MISSION_OBJECT, kNotPickup, kSession, kNoBlast,
	                      BreakCause::OURS, kHost),
	      "a script object is not the map's");
	Check(!MayReportBreak(obj::TEMP_OBJECT, kNotPickup, kSession, kNoBlast,
	                      BreakCause::OURS, kHost),
	      "debris deletes itself and is nobody's business");
	Check(!MayReportBreak(obj::GAME_OBJECT, kPickup, kSession, kNoBlast,
	                      BreakCause::OURS, kHost),
	      "a pickup's own object belongs to docs/pickups.md");
	Check(!MayReportBreak(obj::GAME_OBJECT, kNotPickup, /*haveSession*/ false,
	                      kNoBlast, BreakCause::OURS, kHost),
	      "no session, nothing to say - single player is untouched");
}

void TestLookup() {
	std::printf("finding our copy\n");
	FakePool pool(8);

	// A lamp post, a second one of the same model 10 m away, and a traffic
	// light standing on the same spot as the first.
	uint8_t *post  = pool.Place(0, 100.0f, -200.0f, 15.0f, 1393);
	uint8_t *far   = pool.Place(1, 110.0f, -200.0f, 15.0f, 1393);
	uint8_t *light = pool.Place(2, 100.0f, -200.0f, 15.0f, 1405);
	(void)far;

	bool ambiguous = false;
	Check(FindObjectByIdent(pool.View(), Ident(100.0f, -200.0f, 15.0f, 1393),
	                        &ambiguous) == post,
	      "an exact ident finds its object");
	Check(!ambiguous, "and is not ambiguous");

	Check(FindObjectByIdent(pool.View(), Ident(100.0f, -200.0f, 15.0f, 1405),
	                        nullptr) == light,
	      "the model index separates two objects on the same spot");

	Check(FindObjectByIdent(pool.View(), Ident(100.1f, -200.1f, 15.0f, 1393),
	                        nullptr) == post,
	      "0.14 m off still finds it");
	Check(FindObjectByIdent(pool.View(), Ident(100.3f, -200.0f, 15.0f, 1393),
	                        nullptr) == nullptr,
	      "0.3 m off finds nothing rather than guessing");

	Check(FindObjectByIdent(pool.View(), Ident(600.0f, -700.0f, 65.0f, 1393),
	                        nullptr) == nullptr,
	      "the matcher reads m_objectMatrix, never the live position");

	// Free slots are skipped. This is the case that matters most: the object
	// pool churns constantly, so a stale ident has to miss rather than read a
	// dead slot.
	pool.Free(0);
	Check(FindObjectByIdent(pool.View(), Ident(100.0f, -200.0f, 15.0f, 1393),
	                        nullptr) == nullptr,
	      "a freed slot is not a match");
	pool.Place(0, 100.0f, -200.0f, 15.0f, 1393);

	// Only the map's objects, and never a pickup's collision object - which
	// is also ObjectCreatedBy == GAME_OBJECT, so bIsPickup is the only thing
	// that tells them apart.
	pool.Place(3, 300.0f, 300.0f, 10.0f, 1393, obj::MISSION_OBJECT);
	Check(FindObjectByIdent(pool.View(), Ident(300.0f, 300.0f, 10.0f, 1393),
	                        nullptr) == nullptr,
	      "a script object with the same model is not our lamp post");

	pool.Place(4, 400.0f, 400.0f, 10.0f, 1393, obj::GAME_OBJECT, /*pickup*/ true);
	Check(FindObjectByIdent(pool.View(), Ident(400.0f, 400.0f, 10.0f, 1393),
	                        nullptr) == nullptr,
	      "a pickup's object is skipped even though it is a GAME_OBJECT");

	// Two of the same model inside the tolerance: the nearer one wins and the
	// caller is told it was not clean. The map measurement says this should
	// never happen in Liberty City; the flag is what would make it visible if
	// it ever did.
	uint8_t *near1 = pool.Place(5, 800.0f, 800.0f, 20.0f, 1340);
	uint8_t *near2 = pool.Place(6, 800.1f, 800.0f, 20.0f, 1340);
	(void)near2;
	ambiguous = false;
	Check(FindObjectByIdent(pool.View(), Ident(800.0f, 800.0f, 20.0f, 1340),
	                        &ambiguous) == near1,
	      "two inside the tolerance picks the nearer");
	Check(ambiguous, "and says it was ambiguous");

	// An empty view is not a crash.
	ObjectPoolView empty;
	Check(FindObjectByIdent(empty, Ident(0, 0, 0, 1393), &ambiguous) == nullptr,
	      "an empty pool finds nothing");
	Check(!ambiguous, "and is not ambiguous");
}

void TestStrideIsNotSizeof() {
	std::printf("the stride trap\n");
	// The thing this test exists for: sizeof(CObject) is 0x198 and the pool's
	// slot stride is 0x19C, because re3 declares the pool as
	// CPool<CObject, CCutsceneHead> and CCutsceneHead is one pointer longer.
	// Walking the pool with 0x198 drifts by four bytes per slot and reads a
	// model index out of the middle of the previous entity's matrix.
	Check(obj::OBJECT_POOL_STRIDE == 0x19C, "the stride is 0x19C");
	Check(obj::OBJECT_MATRIX_POS == obj::OBJECT_MATRIX + 0x30,
	      "m_objectMatrix's position is 0x30 into the matrix, as CMatrix says");
	Check(obj::OBJECT_MATRIX_POS + 12 <= obj::UPROOT_LIMIT,
	      "and the whole key fits before m_fUprootLimit");

	FakePool pool(4);
	pool.Place(0, 1.0f, 2.0f, 3.0f, 1393);
	pool.Place(1, 4.0f, 5.0f, 6.0f, 1393);
	// If the stride were wrong, slot 1's key would not read back cleanly.
	Check(FindObjectByIdent(pool.View(), Ident(4.0f, 5.0f, 6.0f, 1393),
	                        nullptr) == pool.Slot(1),
	      "the second slot reads back at its own offset");
}

}  // namespace

int main() {
	std::printf("objecttest: breakable street objects\n\n");
	TestWireLayout();
	TestABulletCannotUprootALampPost();
	TestSameObject();
	TestBreakState();
	TestReplayCount();
	TestWhoReports();
	TestWhoSaysWhereItLanded();
	TestARestingPlaceOffTheWire();
	TestLookup();
	TestStrideIsNotSizeof();

	std::printf("\n%s\n", g_failures == 0 ? "all green"
	                                      : "FAILURES ABOVE");
	return g_failures == 0 ? 0 : 1;
}
