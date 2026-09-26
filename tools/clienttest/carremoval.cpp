// A session car the engine takes away on purpose (game/carremoval.h): who may
// let their engine do it, what the roster does when one does, the parked car
// somebody else took, and - with a copy of the retail exe - every call site and
// test the engine seam leans on, read back out of it.

#include "client.h"
#include "game/addresses.h"
#include "game/carlife.h"
#include "game/carremoval.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

// ---- the rules ------------------------------------------------------------

void TestWhoMayTakeACarAway() {
	std::printf("\ntaking a car away: whose engine may\n");
	constexpr uint8_t N = INVALID_PLAYER;
	Check(MayRemoveCar(2, N, N, false, 2, 0), "its driver's");
	Check(!MayRemoveCar(1, N, 2, true, 2, 2), "nobody else's while somebody drives it, not even the last driver's");
	Check(MayRemoveCar(N, 3, 2, true, 3, 0), "with nobody driving, the custodian's");
	Check(!MayRemoveCar(N, 3, 2, true, 2, 2), "and then not the last driver's");
	Check(MayRemoveCar(N, N, 2, true, 2, 0), "once the custody is over, whoever drove it last");
	Check(!MayRemoveCar(N, N, 2, true, 0, 0), "and then not the host's");
	Check(MayRemoveCar(N, N, 2, false, 0, 0), "the host's when the last driver has left");
	Check(MayRemoveCar(N, N, N, false, 0, 0), "and when nobody has driven it here");
	Check(!MayRemoveCar(N, N, N, false, 1, 0), "nobody but the host's then");
	Check(!MayRemoveCar(N, N, N, false, N, N), "never a machine with no id");
}

void TestWhichReasonAGarageIs() {
	std::printf("\ntaking a car away: which garage is which reason\n");
	Check(RemovalReasonForGarage(GARAGE_CRUSHER) == VEHICLE_REMOVED_CRUSHED, "the crusher");
	Check(RemovalReasonForGarage(GARAGE_COLLECTCARS_1) == VEHICLE_REMOVED_EXPORTED &&
	          RemovalReasonForGarage(GARAGE_COLLECTCARS_3) == VEHICLE_REMOVED_EXPORTED,
	      "all three of Craig's");
	Check(RemovalReasonForGarage(GARAGE_COLLECTSPECIFICCARS) == VEHICLE_REMOVED_COLLECTED,
	      "the police and bank-van garage");
	Check(RemovalReasonForGarage(GARAGE_HIDEOUT_TWO) == VEHICLE_REMOVED_STORED, "a safehouse");
	Check(RemovalReasonForGarage(GARAGE_RESPRAY) == VEHICLE_REMOVED_NONE,
	      "a spray shop takes nothing away");
	Check(IsCraigsGarage(GARAGE_COLLECTCARS_2) && !IsCraigsGarage(GARAGE_COLLECTSPECIFICCARS),
	      "Craig's are the three COLLECTCARS");
}

void TestTheParkedSlot() {
	std::printf("\na parked car somebody takes: its name\n");
	Check(ParkedSlotFor(-1) == 0 && GeneratorForSlot(0) == -1, "no generator is 0 on the wire");
	Check(ParkedSlotFor(0) == 1 && GeneratorForSlot(1) == 0,
	      "generator 0 is 1, so a zeroed claim never names it");
	Check(GeneratorForSlot(ParkedSlotFor(148)) == 148, "the round trip");
}

void TestARemovedCarOfOursIsDestroyed() {
	std::printf("\ntaking a car away: the end of the car on every other machine\n");
	Check(HowToEndCopy(false, false) == CopyEnd::Destroy, "a copy nobody is in is destroyed");
	const bool ours = true, removed = true;
	Check(HowToEndCopy(ours && !removed, false) == CopyEnd::Destroy,
	      "so is our own engine's once it has been taken away");
	Check(HowToEndCopy(false, true) == CopyEnd::HandToEngine,
	      "but never from under the local player");
}

// ---- the roster -----------------------------------------------------------

struct Rec {
	int            despawns       = 0;
	int            ownReleases    = 0;
	int            spawns         = 0;
	uint16_t       takenOver      = 0;
	int            takeOvers      = 0;
	VehicleRemoval pending[4];
	uint8_t        pendingCount   = 0;
	VehicleRemover removers[64];
	uint8_t        removerCount   = 0;
	int32_t        localHandle    = -1;
	uint16_t       parkedSlot     = 0;
};
Rec g_rec;

bool RecModelReady(uint16_t) { return true; }
void RecRequestModel(uint16_t) {}
bool RecSpawn(RemoteVehicle &v) {
	++g_rec.spawns;
	v.poolHandle = 1000 + v.netId;
	return true;
}
void RecDespawn(RemoteVehicle &v) {
	++g_rec.despawns;
	v.poolHandle = -1;
}
void RecRelease(RemoteVehicle &) { ++g_rec.ownReleases; }
void RecNoteRemovers(const VehicleRemover *rows, uint8_t count) {
	g_rec.removerCount = count;
	for (uint8_t i = 0; i < count; ++i)
		g_rec.removers[i] = rows[i];
}
uint8_t RecDrain(VehicleRemoval *out, uint8_t max) {
	uint8_t n = 0;
	for (; n < g_rec.pendingCount && n < max; ++n)
		out[n] = g_rec.pending[n];
	g_rec.pendingCount = 0;
	return n;
}
void RecTakeOver(uint16_t slot) {
	++g_rec.takeOvers;
	g_rec.takenOver = slot;
}
int32_t RecLocalHandle() { return g_rec.localHandle; }
bool RecIdentity(VehicleIdentity &out) {
	out            = VehicleIdentity{};
	out.modelId    = 116;
	out.parkedSlot = g_rec.parkedSlot;
	return true;
}

WorldBridge Bridge() {
	g_rec = Rec{};
	WorldBridge b;
	b.RequestModel               = &RecRequestModel;
	b.IsModelReady               = &RecModelReady;
	b.SpawnRemoteVehicle         = &RecSpawn;
	b.DespawnRemoteVehicle       = &RecDespawn;
	b.ReleaseOwnVehicle          = &RecRelease;
	b.NoteVehicleRemovers        = &RecNoteRemovers;
	b.DrainVehicleRemovals       = &RecDrain;
	b.TakeOverParkedCar          = &RecTakeOver;
	b.SampleLocalVehicleHandle   = &RecLocalHandle;
	b.SampleLocalVehicleIdentity = &RecIdentity;
	return b;
}

template <class T>
Message Wrap(const T &pkt, uint8_t channel = CH_EVENT) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = channel;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

S_Welcome Welcome(uint8_t playerId, uint8_t host) {
	S_Welcome w;
	InitHeader(w, 1000);
	w.playerId     = playerId;
	w.netId        = uint16_t(100 + playerId);
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hour         = 12;
	w.hostPlayerId = host;
	return w;
}

S_VehicleSpawn Spawn(uint16_t netId, uint16_t parkedSlot = 0) {
	S_VehicleSpawn s;
	InitHeader(s, 1000);
	s.netId      = netId;
	s.modelId    = 116;
	s.pos        = {10.0f, 20.0f, 30.0f};
	s.rot        = {0.0f, 0.0f, 0.0f, 1.0f};
	s.health     = 1000.0f;
	s.extra1     = -1;
	s.extra2     = -1;
	s.parkedSlot = parkedSlot;
	return s;
}

S_EnterVehicle Enter(uint8_t playerId, uint16_t netId) {
	S_EnterVehicle e{};
	InitHeader(e, 1000);
	e.playerId   = playerId;
	e.body.netId = netId;
	return e;
}

S_ExitVehicle Exit(uint8_t playerId, uint16_t netId) {
	S_ExitVehicle e{};
	InitHeader(e, 1000);
	e.playerId = playerId;
	e.netId    = netId;
	return e;
}

S_PlayerJoin Join(uint8_t playerId) {
	S_PlayerJoin j{};
	InitHeader(j, 1000);
	j.playerId = playerId;
	j.netId    = uint16_t(100 + playerId);
	std::strcpy(j.nick, "other");
	return j;
}

const VehicleRemover *RemoverFor(uint16_t netId) {
	for (uint8_t i = 0; i < g_rec.removerCount; ++i)
		if (g_rec.removers[i].netId == netId)
			return &g_rec.removers[i];
	return nullptr;
}

void TestTheLastDriverHoldsAParkedCar() {
	std::printf("\ntaking a car away: the roster names the machine that may\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	c.HandleMessage(Wrap(Join(1)));
	c.HandleMessage(Wrap(Spawn(40)));
	c.Tick();
	const VehicleRemover *r = RemoverFor(40);
	Check(r != nullptr && r->weMay, "a car nobody has driven is the host's, and we host");

	c.HandleMessage(Wrap(Enter(1, 40)));
	c.Tick();
	r = RemoverFor(40);
	Check(r != nullptr && !r->weMay, "somebody else driving it: not ours");

	c.HandleMessage(Wrap(Exit(1, 40)));
	c.Tick();
	r = RemoverFor(40);
	Check(r != nullptr && !r->weMay, "parked by him, it stays his, host or not");
	const RemoteVehicle *v = c.VehicleByNetId(40);
	Check(v && v->lastDriverPlayerId == 1, "the roster remembers who drove it last");
}

void TestOurEngineTakingItIsSaidAndNotRebuilt() {
	std::printf("\ntaking a car away: our own engine did it\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	c.HandleMessage(Wrap(Spawn(41)));
	c.Tick();
	Check(g_rec.spawns == 1, "built once");

	g_rec.pending[0]   = VehicleRemoval{41, VEHICLE_REMOVED_CRUSHED};
	g_rec.pendingCount = 1;
	c.Tick();
	const RemoteVehicle *v = c.VehicleByNetId(41);
	Check(v && v->removed, "the row is marked gone");
	Check(!v || !v->spawnPending, "and is not waiting to be rebuilt");
	c.Tick();
	c.Tick();
	Check(g_rec.spawns == 1, "nothing rebuilds it");
	Check(RemoverFor(41) == nullptr, "and it is off the table the garages read");
}

void TestTheHoldersRemovalEndsOurOwnCarToo() {
	std::printf("\ntaking a car away: somebody else's engine did it\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	c.HandleMessage(Wrap(Spawn(42)));
	c.HandleMessage(Wrap(Spawn(43)));
	c.Tick();

	S_VehicleRemoved removed{};
	InitHeader(removed, 2000);
	removed.playerId               = 1;
	removed.body.netId             = 42;
	removed.body.reason            = VEHICLE_REMOVED_EXPORTED;
	c.HandleMessage(Wrap(removed));
	const RemoteVehicle *v = c.VehicleByNetId(42);
	Check(v && v->removed, "the row is marked taken away");
	Check(c.VehicleByNetId(43) && !c.VehicleByNetId(43)->removed, "and no other");
	S_VehicleDespawn d;
	InitHeader(d, 2000);
	d.netId = 42;
	c.HandleMessage(Wrap(d));
	Check(g_rec.despawns == 1, "the copy is destroyed");

	Check(c.VehicleByNetId(42) == nullptr, "and the row is gone");
}

void TestAParkedCarClaimAndSpawnNameTheGenerator() {
	std::printf("\na parked car somebody takes: the claim and the spawn\n");
	Client c;
	c.SetBridge(Bridge());
	c.HandleMessage(Wrap(Welcome(0, 0)));
	c.HandleMessage(Wrap(Spawn(44, 13)));
	Check(g_rec.takeOvers == 1 && g_rec.takenOver == 13,
	      "a spawn naming a generator has ours on it let go");
	c.HandleMessage(Wrap(Spawn(45)));
	Check(g_rec.takeOvers == 1, "a spawn naming none touches no generator");

	VehicleIdentity id{};
	Check(id.parkedSlot == 0, "an identity names no generator unless it is sampled");
	EnterVehicleBody body{};
	Check(body.parkedSlot == 0, "and a zeroed claim names none either");
}

// ---- against the real exe --------------------------------------------------

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

uint8_t Byte(const std::vector<uint8_t> &img, uint32_t va) { return img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uint32_t va) {
	const size_t o = va - IMAGE_BASE;
	return uint32_t(img[o]) | uint32_t(img[o + 1]) << 8 | uint32_t(img[o + 2]) << 16 |
	       uint32_t(img[o + 3]) << 24;
}

bool Bytes(const std::vector<uint8_t> &img, uint32_t va, std::initializer_list<int> want) {
	uint32_t at = va;
	for (int b : want) {
		if (b >= 0 && Byte(img, at) != static_cast<uint8_t>(b))
			return false;
		++at;
	}
	return true;
}

bool CallsTo(const std::vector<uint8_t> &img, uint32_t site, uint32_t to) {
	return Byte(img, site) == 0xE8 && site + 5 + Dword(img, site + 1) == to;
}

void TestAgainstTheImage() {
	std::printf("\ntaking a car away: against the retail exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "crusher, the crane and the garages against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	Check(CallsTo(img, GARAGE_DESTROY_MISSION_CALL, DestroyVehicleAndDriverAndPassengers) &&
	          CallsTo(img, GARAGE_DESTROY_COLLECTED_CALL, DestroyVehicleAndDriverAndPassengers) &&
	          CallsTo(img, GARAGE_DESTROY_CRAIG_CALL, DestroyVehicleAndDriverAndPassengers) &&
	          CallsTo(img, GARAGE_DESTROY_CRUSHER_CALL, DestroyVehicleAndDriverAndPassengers),
	      "the four garage deliveries call DestroyVehicleAndDriverAndPassengers");
	Check(Bytes(img, DestroyVehicleAndDriverAndPassengers,
	            {0x53, 0x8B, 0x5C, 0x24, 0x08, 0x8B, 0x83, 0xA4, 0x01, 0x00, 0x00}),
	      "which takes its car off the stack and reads the driver first");
	Check(CallsTo(img, 0x00423B3C, CGarage__MarkThisCarAsCollectedForCraig),
	      "Craig's marks the car collected right before it");
	Check(Bytes(img, 0x00426E91, {0x09, 0x1C, 0x85}) &&
	          Dword(img, 0x00426E94) == CGarages__CarTypesCollected,
	      "into CGarages::CarTypesCollected");
	Check(Bytes(img, 0x00424174, {0xFF, 0x05, 0x50, 0x30, 0x94, 0x00}),
	      "the crusher's call is followed by CarsCrushed++");
	Check(CallsTo(img, 0x00423BD6, FindPlayerVehicle) &&
	          Bytes(img, GARAGE_CRAIG_MISSION_CAR_TEST, {0x80, 0xB8, 0xF4, 0x01, 0x00, 0x00, 0x02}),
	      "Craig refuses the local player's MISSION_VEHICLE");
	Check(CallsTo(img, HIDEOUT_STORE_UPDATE_CALL, CGarage__StoreAndRemoveCarsForThisHideout) &&
	          CallsTo(img, HIDEOUT_STORE_CLOSE_CALL, CGarage__StoreAndRemoveCarsForThisHideout),
	      "both safehouse stores");
	Check(Bytes(img, 0x0042794C, {0x80, 0xBD, 0xF4, 0x01, 0x00, 0x00, 0x02, 0x0F, 0x84}),
	      "which skip a MISSION_VEHICLE");
	Check(Bytes(img, 0x00427A08, {0xC2, 0x08, 0x00}), "and take two arguments");
	Check(CallsTo(img, CRANE_FIND_VEHICLES_CALL, CCrane__FindCarInSectorList) &&
	          CallsTo(img, CRANE_FIND_OVERLAP_CALL, CCrane__FindCarInSectorList),
	      "both of the crane's searches");
	Check(Bytes(img, CCrane__FindCarInSectorList + 0x15, {0x66, 0xA1}) &&
	          Dword(img, CCrane__FindCarInSectorList + 0x17) == CWorld__ms_nCurrentScanCode &&
	          Bytes(img, CCrane__FindCarInSectorList + 0x1B, {0x66, 0x8B, 0x55, 0x58}),
	      "which skip a car whose scan code is already the current one");
	Check(CallsTo(img, CRANE_MILITARY_REMOVE_CALL, CWorld__Remove) &&
	          Bytes(img, 0x00544257, {0x80, 0x7B, 0x7C, 0x00}),
	      "the military crane's delivery removes the car from the world");
	Check(Bytes(img, 0x00544B93, {0x83, 0x0D}) &&
	          Dword(img, 0x00544B95) == CCranes__CarsCollectedMilitaryCrane,
	      "and its list is CCranes::CarsCollectedMilitaryCrane");
	Check(Bytes(img, 0x0052CF5A, {0x83, 0xB8, 0x24, 0x02, 0x00, 0x00, 0x01}) &&
	          Bytes(img, 0x0052CF74, {0xC7, 0x80, 0x24, 0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00}) &&
	          Bytes(img, 0x0052CF6B, {0x83, 0xFE, 0x7A}),
	      "the automobile constructor locks three models CARLOCK_LOCKED_INITIALLY");
	Check(Bytes(img, 0x00542C0D, {0x81, 0x43, 0x20, 0x60, 0xEA, 0x00, 0x00}) &&
	          Bytes(img, 0x00542C14, {0x83, 0x4B, 0x24, 0xFF}) &&
	          Bytes(img, 0x00542C18, {0xC6, 0x43, 0x2A, 0x01}),
	      "a generator lets go of a taken car: a minute, no handle, blocking");
	Check(Bytes(img, CEntity__PruneReferences, {0x8B, 0x51, 0x60}),
	      "PruneReferences walks the entity's reference list");
}

} // namespace

int RunCarRemovalTests() {
	std::printf("\n");
	TestWhoMayTakeACarAway();
	TestWhichReasonAGarageIs();
	TestTheParkedSlot();
	TestARemovedCarOfOursIsDestroyed();
	TestTheLastDriverHoldsAParkedCar();
	TestOurEngineTakingItIsSaidAndNotRebuilt();
	TestTheHoldersRemovalEndsOurOwnCarToo();
	TestAParkedCarClaimAndSpawnNameTheGenerator();
	TestAgainstTheImage();
	return g_failures;
}
