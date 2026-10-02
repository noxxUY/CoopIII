// Custom skins: coopiii/skin.h, client/src/skinsync.h and the engine half's
// addresses (game/skin.h).
//
// What can be checked without a game: a skin's pieces on the wire, putting
// them back together and what is refused, the BMP reader, our skin going out
// and theirs coming in through Client, and - with a retail exe handed over -
// every instruction the engine half rests on.

#include "client.h"
#include "game/addresses.h"
#include "skinsync.h"

#include <coopiii/skin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_skinFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_skinFailures;
}

// A test picture: `colours` distinct colours, laid out so every row differs.
std::vector<uint8_t> Picture(uint32_t w, uint32_t h, uint32_t colours) {
	std::vector<uint8_t> rgba(size_t(w) * h * 4);
	for (uint32_t i = 0; i < w * h; ++i) {
		const uint32_t c = (i * 7 + i / w) % colours;
		rgba[i * 4 + 0]  = uint8_t(c * 3);
		rgba[i * 4 + 1]  = uint8_t(c >> 2);
		rgba[i * 4 + 2]  = uint8_t(255 - c);
		rgba[i * 4 + 3]  = 0x80;   // ignored: skins are opaque
	}
	return rgba;
}

bool SameColours(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i += 4)
		if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || b[i + 3] != 0xFF)
			return false;
	return true;
}

std::vector<SkinChunk> Pieces(const Skin &skin) {
	std::vector<SkinChunk> out;
	uint32_t               at = 0;
	SkinChunk              piece;
	while (CutSkinPiece(skin, at, piece))
		out.push_back(piece);
	return out;
}

void TestTheWire() {
	std::printf("\na skin on the wire\n");
	Check(SkinSideOk(16) && SkinSideOk(256) && SkinSideOk(512) && !SkinSideOk(8) &&
	          !SkinSideOk(1024) && !SkinSideOk(255) && !SkinSideOk(0),
	      "a side is a power of two from 16 to 512");
	Check(SkinPayloadBytes(SKIN_FORMAT_RGB, 256, 256) == 196608 &&
	          SkinPayloadBytes(SKIN_FORMAT_PALETTE, 256, 256) == 768 + 65536 &&
	          SkinPayloadBytes(SKIN_FORMAT_RGB, 300, 256) == 0 &&
	          SkinPayloadBytes(7, 256, 256) == 0,
	      "the payload's size follows from the format and the size, and nothing else is one");
	Check(SKIN_MAX_BYTES == 512u * 512u * 3u, "the most a skin can be is 768 KiB");

	const std::vector<uint8_t> few = Picture(256, 256, 200);
	Skin                       pal;
	Check(EncodeSkin(256, 256, few.data(), 5, "playa", pal) &&
	          pal.info.format == SKIN_FORMAT_PALETTE && pal.info.bytes == 768 + 65536 &&
	          pal.info.hash == SkinHash(pal.payload.data(), pal.payload.size()) &&
	          std::strcmp(pal.info.name, "playa") == 0,
	      "200 colours go with a palette, the game's own skins' shape");
	std::vector<uint8_t> back;
	Check(DecodeSkin(pal, back) && SameColours(few, back), "and come back the same, opaque");

	const std::vector<uint8_t> many = Picture(128, 64, 1000);
	Skin                       rgb;
	Check(EncodeSkin(128, 64, many.data(), 6, "hd", rgb) && rgb.info.format == SKIN_FORMAT_RGB &&
	          rgb.info.bytes == 128 * 64 * 3,
	      "more than 256 go as RGB");
	Check(DecodeSkin(rgb, back) && SameColours(many, back), "and come back the same");
	Check(!EncodeSkin(100, 64, many.data(), 7, "odd", rgb), "a side that is not one is refused");
	Check(!EncodeSkin(128, 64, many.data(), 0, "zero", rgb), "and so is serial 0");

	SkinInfo info = pal.info;
	Check(SkinInfoOk(info), "a whole skin's info is fine");
	info.bytes += 1;
	Check(!SkinInfoOk(info), "one byte more than its size is not");
	info        = pal.info;
	info.width  = 1024;
	Check(!SkinInfoOk(info), "nor is a side past 512");
	info        = pal.info;
	info.format = 9;
	Check(!SkinInfoOk(info), "nor a format nobody knows");
	Skin def = DefaultSkin(3, "$$\"\"");
	Check(SkinInfoOk(def.info) && def.info.format == SKIN_FORMAT_DEFAULT && def.info.bytes == 0,
	      "the default skin is a skin with nothing in it");
	info       = def.info;
	info.width = 16;
	Check(!SkinInfoOk(info), "and one with a size is not the default skin");

	char name[SKIN_NAME_LEN];
	std::memset(name, 'x', sizeof name);
	name[3] = '\n';
	CleanSkinName(name);
	Check(name[3] == '_' && name[SKIN_NAME_LEN - 1] == '\0' && std::strlen(name) == 31,
	      "a name is printable and terminated, whatever came");

	const std::vector<SkinChunk> pieces = Pieces(pal);
	Check(pieces.size() == 65 && pieces[0].length == SKIN_CHUNK_BYTES &&
	          pieces.back().length == (768 + 65536) % SKIN_CHUNK_BYTES &&
	          pieces.back().offset == 64 * SKIN_CHUNK_BYTES,
	      "a 256x256 skin with a palette is 65 pieces, the last one short");
	Check(Pieces(def).size() == 1 && Pieces(def)[0].length == 0,
	      "the default skin is one empty piece");
}

void TestPuttingItBackTogether() {
	std::printf("\nputting a skin back together\n");
	const std::vector<uint8_t> px = Picture(64, 64, 300);
	Skin                       skin;
	EncodeSkin(64, 64, px.data(), 9, "dude", skin);
	const std::vector<SkinChunk> pieces = Pieces(skin);
	Check(pieces.size() == 12, "(a 64x64 RGB skin is 12 pieces)");

	SkinAssembly in;
	bool         partial = true;
	for (size_t i = 0; i + 1 < pieces.size(); ++i)
		partial = partial && in.Take(pieces[i]) == SkinAssembly::Step::Partial;
	Check(partial && in.Take(pieces.back()) == SkinAssembly::Step::Complete,
	      "in order, it is whole on the last piece");
	const Skin done = in.Finished();
	Check(done.payload == skin.payload && done.info.hash == skin.info.hash, "and is what was sent");

	Check(in.Take(pieces[1]) == SkinAssembly::Step::Rejected,
	      "a skin can only start with its first piece");
	in.Take(pieces[0]);
	Check(in.Take(pieces[2]) == SkinAssembly::Step::Rejected && !in.Open(),
	      "a piece that skips one throws the half away");
	in.Take(pieces[0]);
	SkinChunk shortPiece = pieces[1];
	shortPiece.length    = 100;
	Check(in.Take(shortPiece) == SkinAssembly::Step::Rejected,
	      "so does one shorter than it must be");
	SkinChunk liar = pieces[0];
	liar.info.bytes = 9999;
	Check(in.Take(liar) == SkinAssembly::Step::Rejected, "and one whose size is not a skin's");

	// A wrong hash only shows at the end.
	Skin bad = skin;
	bad.info.hash ^= 1;
	SkinAssembly    check;
	SkinAssembly::Step last = SkinAssembly::Step::Partial;
	for (const SkinChunk &p : Pieces(bad))
		last = check.Take(p);
	Check(last == SkinAssembly::Step::Rejected, "a payload that does not hash right is refused");

	// A new skin halfway through the old one starts over.
	Skin next;
	EncodeSkin(64, 64, Picture(64, 64, 20).data(), 10, "dude2", next);
	SkinAssembly swap;
	swap.Take(pieces[0]);
	swap.Take(pieces[1]);
	last = SkinAssembly::Step::Partial;
	for (const SkinChunk &p : Pieces(next))
		last = swap.Take(p);
	Check(last == SkinAssembly::Step::Complete && swap.Finished().info.serial == 10,
	      "a new serial halfway through takes over from its first piece");

	SkinAssembly def;
	Check(def.Take(Pieces(DefaultSkin(4, "x"))[0]) == SkinAssembly::Step::Complete,
	      "the default skin is whole at once");

	SkinBudget budget;
	uint32_t   spent = 0;
	while (budget.Spend(1000, SKIN_CHUNK_BYTES))
		spent += SKIN_CHUNK_BYTES;
	Check(spent == SKIN_BURST_BYTES, "a budget lets a burst go at once");
	Check(!budget.Spend(1001, SKIN_CHUNK_BYTES) && budget.Spend(1010, SKIN_CHUNK_BYTES),
	      "and then a piece every 8 ms or so, 128 KiB a second");
}

// ---- the BMP reader ---------------------------------------------------------

void Put32(std::vector<uint8_t> &f, size_t at, uint32_t v) {
	for (int i = 0; i < 4; ++i)
		f[at + i] = uint8_t(v >> (8 * i));
}
void Put16(std::vector<uint8_t> &f, size_t at, uint16_t v) {
	f[at]     = uint8_t(v);
	f[at + 1] = uint8_t(v >> 8);
}

// A BMP of `rgba` at `bits` per pixel, bottom-up unless `topDown`.
std::vector<uint8_t> Bmp(uint32_t w, uint32_t h, uint16_t bits, const std::vector<uint8_t> &rgba,
                         bool topDown = false) {
	const uint32_t paletteN = bits <= 8 ? (1u << bits) : 0;
	const size_t   stride   = ((size_t(w) * bits + 31) / 32) * 4;
	const size_t   pixelsAt = 14 + 40 + paletteN * 4;
	std::vector<uint8_t> f(pixelsAt + stride * h, 0);
	f[0] = 'B';
	f[1] = 'M';
	Put32(f, 2, uint32_t(f.size()));
	Put32(f, 10, uint32_t(pixelsAt));
	Put32(f, 14, 40);
	Put32(f, 18, w);
	Put32(f, 22, topDown ? uint32_t(-int32_t(h)) : h);
	Put16(f, 26, 1);
	Put16(f, 28, bits);
	// A palette of the first colours met, for the 4- and 8-bit ones.
	std::vector<uint32_t> pal;
	for (uint32_t y = 0; y < h; ++y) {
		uint8_t *row = f.data() + pixelsAt + stride * (topDown ? y : h - 1 - y);
		for (uint32_t x = 0; x < w; ++x) {
			const uint8_t *p = &rgba[(size_t(y) * w + x) * 4];
			if (bits >= 24) {
				uint8_t *d = row + x * (bits / 8);
				d[0] = p[2];
				d[1] = p[1];
				d[2] = p[0];
				continue;
			}
			const uint32_t key = p[0] | p[1] << 8 | p[2] << 16;
			size_t         i   = 0;
			while (i < pal.size() && pal[i] != key)
				++i;
			if (i == pal.size())
				pal.push_back(key);
			if (bits == 8)
				row[x] = uint8_t(i);
			else
				row[x / 2] |= uint8_t((i & 15) << ((x & 1) ? 0 : 4));
		}
	}
	for (size_t i = 0; i < pal.size() && i < paletteN; ++i) {
		f[54 + i * 4 + 0] = uint8_t(pal[i] >> 16);
		f[54 + i * 4 + 1] = uint8_t(pal[i] >> 8);
		f[54 + i * 4 + 2] = uint8_t(pal[i]);
	}
	return f;
}

void TestTheBmpReader() {
	std::printf("\nreading a skin out of the skins folder\n");
	const std::vector<uint8_t> px8 = Picture(32, 16, 200);
	SkinImage                  img;
	std::vector<uint8_t>       f = Bmp(32, 16, 8, px8);
	Check(ReadSkinBmp(f.data(), f.size(), img) && img.width == 32 && img.height == 16 &&
	          SameColours(px8, img.rgba),
	      "8 bits with a palette, bottom-up, the way the game's playa skin is");
	const std::vector<uint8_t> px = Picture(32, 16, 900);
	f = Bmp(32, 16, 24, px);
	Check(ReadSkinBmp(f.data(), f.size(), img) && SameColours(px, img.rgba),
	      "24 bits, the way its default skin is");
	f = Bmp(32, 16, 32, px, true);
	Check(ReadSkinBmp(f.data(), f.size(), img) && SameColours(px, img.rgba),
	      "32 bits, top-down");
	const std::vector<uint8_t> px4 = Picture(16, 16, 12);
	f = Bmp(16, 16, 4, px4);
	Check(ReadSkinBmp(f.data(), f.size(), img) && SameColours(px4, img.rgba),
	      "and 4 bits");

	f = Bmp(32, 16, 24, px);
	Put32(f, 30, 1);   // BI_RLE8
	Check(!ReadSkinBmp(f.data(), f.size(), img), "a compressed one is refused");
	f = Bmp(32, 16, 24, px);
	Check(!ReadSkinBmp(f.data(), f.size() - 1, img), "so is one cut short");
	f = Bmp(32, 16, 24, px);
	Put32(f, 18, 33);
	Check(!ReadSkinBmp(f.data(), f.size(), img), "and one whose side is not a skin's");
	f = Bmp(32, 16, 24, px);
	f[0] = 'X';
	Check(!ReadSkinBmp(f.data(), f.size(), img) && !ReadSkinBmp(nullptr, 0, img),
	      "and anything that is not a BMP");
}

// ---- SkinSync ---------------------------------------------------------------

struct SkinRec {
	char                 localName[SKIN_NAME_LEN] = {};
	bool                 haveFile = false;
	SkinImage            file;
	int                  loads = 0;
	int                  sets[MAX_PLAYERS] = {};
	bool                 custom[MAX_PLAYERS] = {};
	SkinImage            worn[MAX_PLAYERS];
	int                  places = 0;
	int32_t              peds[MAX_PLAYERS] = {};
	std::vector<C_PlayerSkin> sent;
};
SkinRec g_skin;

bool RecReadName(char (&name)[SKIN_NAME_LEN]) {
	std::memcpy(name, g_skin.localName, sizeof name);
	return name[0] != '\0';
}
bool RecLoad(const char *, SkinImage &out) {
	++g_skin.loads;
	if (!g_skin.haveFile)
		return false;
	out = g_skin.file;
	return true;
}
void RecSet(uint8_t id, const SkinImage *img) {
	++g_skin.sets[id];
	g_skin.custom[id] = img != nullptr;
	g_skin.worn[id]   = img ? *img : SkinImage{};
}
void RecPlace(const int32_t (&peds)[MAX_PLAYERS]) {
	++g_skin.places;
	std::memcpy(g_skin.peds, peds, sizeof peds);
}
void RecSend(void *, const void *bytes, size_t len, Channel ch) {
	if (len == sizeof(C_PlayerSkin) && ch == CH_EVENT) {
		C_PlayerSkin p;
		std::memcpy(&p, bytes, sizeof p);
		g_skin.sent.push_back(p);
	}
}

SkinBridge SkinStub() {
	SkinBridge b;
	b.ReadLocalSkinName = &RecReadName;
	b.LoadLocalSkin     = &RecLoad;
	b.SetRemoteSkin     = &RecSet;
	b.PlaceRemoteSkins  = &RecPlace;
	return b;
}

S_PlayerSkin Relay(uint8_t from, const C_PlayerSkin &in) {
	S_PlayerSkin out;
	InitHeader(out, 100);
	out.playerId = from;
	out.chunk    = in.chunk;
	return out;
}

void TestOursGoesOut() {
	std::printf("\nour skin going out\n");
	g_skin = SkinRec{};
	std::strcpy(g_skin.localName, "playa");
	g_skin.haveFile    = true;
	g_skin.file.width  = 256;
	g_skin.file.height = 256;
	g_skin.file.rgba   = Picture(256, 256, 120);

	const SkinBridge bridge = SkinStub();
	SkinSync         ours;
	ours.Bind(&bridge, &RecSend, nullptr);
	ours.Tick(INVALID_PLAYER, 0);
	Check(g_skin.sent.empty() && g_skin.loads == 0, "nothing before the session has named us");
	ours.Tick(0, 1000);
	Check(g_skin.loads == 1 && ours.Ours().format == SKIN_FORMAT_PALETTE &&
	          std::strcmp(ours.Ours().name, "playa") == 0,
	      "the skin we wear is read and goes as a palette");
	Check(g_skin.sent.size() == SKIN_BURST_BYTES / SKIN_CHUNK_BYTES && ours.Sending(),
	      "a burst of pieces at once, and the rest held back");
	uint32_t now = 1000;
	while (ours.Sending() && now < 10000)
		ours.Tick(0, now += 16);
	Check(!ours.Sending() && g_skin.sent.size() == 65 && now <= 1000 + 700,
	      "all 65 within a second at 60 frames a second");

	SkinSync theirs;
	theirs.Bind(&bridge, &RecSend, nullptr);
	for (const C_PlayerSkin &p : g_skin.sent)
		theirs.OnSkin(Relay(3, p), 1, true);
	Check(g_skin.sets[3] == 1 && g_skin.custom[3] && g_skin.worn[3].width == 256 &&
	          SameColours(g_skin.file.rgba, g_skin.worn[3].rgba),
	      "another machine puts it on that player, pixel for pixel");
	Check(theirs.WornBy(3) && theirs.WornBy(3)->serial == ours.Ours().serial,
	      "and keeps which skin it is");

	// Unchanged, nothing more goes.
	const size_t before = g_skin.sent.size();
	ours.Tick(0, now += 2000);
	Check(g_skin.sent.size() == before && g_skin.loads == 1, "the same skin is not sent twice");

	// Player Setup changes it halfway through: the new one takes over.
	std::strcpy(g_skin.localName, "bigdude");
	g_skin.file.width  = 256;
	g_skin.file.height = 256;
	g_skin.file.rgba   = Picture(256, 256, 900);
	g_skin.sent.clear();
	ours.Tick(0, now += 2000);
	const uint32_t firstSerial = ours.Ours().serial;
	Check(ours.Sending() && ours.Ours().format == SKIN_FORMAT_RGB,
	      "(a 256x256 skin of 900 colours is on its way as RGB)");
	std::strcpy(g_skin.localName, "other");
	g_skin.file.width  = 64;
	g_skin.file.height = 64;
	g_skin.file.rgba   = Picture(64, 64, 900);
	ours.Tick(0, now += 2000);
	Check(ours.Ours().serial != firstSerial && g_skin.sent.back().chunk.info.serial ==
	                                               ours.Ours().serial,
	      "a skin changed while the last was going goes instead of it");
	while (ours.Sending())
		ours.Tick(0, now += 16);
	g_skin.sets[3] = 0;
	for (const C_PlayerSkin &p : g_skin.sent)
		theirs.OnSkin(Relay(3, p), 1, true);
	Check(g_skin.sets[3] == 1 && g_skin.worn[3].width == 64 &&
	          SameColours(g_skin.file.rgba, g_skin.worn[3].rgba),
	      "and the receiver, having thrown the first half away, wears the new one");

	// The default skin, or a file that will not read, goes as the default.
	g_skin.haveFile = false;
	std::strcpy(g_skin.localName, "$$\"\"");
	g_skin.sent.clear();
	ours.Tick(0, now += 2000);
	Check(g_skin.sent.size() == 1 && g_skin.sent[0].chunk.info.format == SKIN_FORMAT_DEFAULT,
	      "the default skin is one empty piece");
	theirs.OnSkin(Relay(3, g_skin.sent[0]), 1, true);
	Check(!g_skin.custom[3] && !theirs.WornBy(3),
	      "and puts that player back in the default skin, not in ours");

	// The rule off: nothing goes, nobody wears a custom skin.
	std::strcpy(g_skin.localName, "playa");
	g_skin.haveFile = true;
	g_skin.sent.clear();
	ours.SetRule(SKIN_RULE_OFF);
	ours.Tick(0, now += 2000);
	Check(g_skin.sent.empty(), "with the session's rule off, ours stays home");
	ours.SetRule(SKIN_RULE_SYNC);
	ours.Tick(0, now += 2000);
	Check(!g_skin.sent.empty() && g_skin.sent[0].chunk.offset == 0,
	      "and on again, it goes again from the first piece");
	ours.SetRule(7);
	Check(ours.Rule() == SKIN_RULE_OFF, "a rule nobody knows is off");
}

void TestTheirsComeIn() {
	std::printf("\ntheirs coming in\n");
	g_skin = SkinRec{};
	const SkinBridge bridge = SkinStub();
	SkinSync         s;
	s.Bind(&bridge, &RecSend, nullptr);
	Skin skin;
	const std::vector<uint8_t> px = Picture(32, 32, 50);
	EncodeSkin(32, 32, px.data(), 1, "a", skin);
	const std::vector<SkinChunk> pieces = Pieces(skin);
	auto send = [&](uint8_t from, uint8_t local, bool known) {
		for (const SkinChunk &c : pieces) {
			S_PlayerSkin p;
			InitHeader(p, 1);
			p.playerId = from;
			p.chunk    = c;
			s.OnSkin(p, local, known);
		}
	};
	send(2, 2, true);
	Check(g_skin.sets[2] == 0, "a skin under our own id is not put on anybody");
	send(4, 0, false);
	Check(g_skin.sets[4] == 0, "nor one for a player the roster does not have");
	S_PlayerSkin odd;
	InitHeader(odd, 1);
	odd.playerId = 200;
	odd.chunk    = pieces[0];
	s.OnSkin(odd, 0, true);
	send(5, 0, true);
	Check(g_skin.sets[5] == 1 && g_skin.custom[5], "one for somebody we know is");
	s.SetRule(SKIN_RULE_OFF);
	Check(!g_skin.custom[5], "the rule switched off puts him back in the default skin");
	g_skin.sets[5] = 0;
	send(5, 0, true);
	Check(g_skin.sets[5] == 0, "and nothing that comes while it is off is worn");
	s.SetRule(SKIN_RULE_SYNC);
	send(5, 0, true);
	Check(g_skin.custom[5], "on again, the server's next sending is");
	s.Forget(5);
	Check(!g_skin.custom[5] && !s.WornBy(5), "and a player who leaves takes his skin with him");
}

// Through Client: the dispatch, the roster and the rules.
Message WrapSkin(const S_PlayerSkin &p) {
	Message m;
	m.opcode  = S_PlayerSkin::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof p);
	std::memcpy(m.data.data(), &p, sizeof p);
	return m;
}

template <class T>
Message WrapAny(const T &p) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof p);
	std::memcpy(m.data.data(), &p, sizeof p);
	return m;
}

void TestThroughTheClient() {
	std::printf("\nskins through the client\n");
	g_skin = SkinRec{};
	WorldBridge b;
	b.skins = SkinStub();
	Client c;
	c.SetBridge(b);

	S_Welcome w;
	InitHeader(w, 1);
	w.playerId     = 0;
	w.netId        = 100;
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hostPlayerId = INVALID_PLAYER;
	c.HandleMessage(WrapAny(w));
	S_PlayerJoin j;
	InitHeader(j, 1);
	j.playerId = 1;
	j.netId    = 201;
	std::strcpy(j.nick, "bob");
	c.HandleMessage(WrapAny(j));
	for (int &n : g_skin.sets)
		n = 0;

	Skin skin;
	const std::vector<uint8_t> px = Picture(128, 128, 64);
	EncodeSkin(128, 128, px.data(), 1, "playa", skin);
	for (const SkinChunk &piece : Pieces(skin)) {
		S_PlayerSkin p;
		InitHeader(p, 1);
		p.playerId = 1;
		p.chunk    = piece;
		c.HandleMessage(WrapSkin(p));
	}
	Check(g_skin.sets[1] == 1 && g_skin.custom[1] && SameColours(px, g_skin.worn[1].rgba),
	      "bob's skin reaches the engine half for bob alone");
	c.Tick();
	Check(g_skin.places > 0 && g_skin.peds[0] == -1 && g_skin.peds[1] == -1,
	      "every frame the engine half is told whose ped is whose, ours never");

	S_SessionRules rules;
	InitHeader(rules, 1);
	rules.maxWanted = 6;
	rules.skins     = SKIN_RULE_OFF;
	c.HandleMessage(WrapAny(rules));
	Check(!g_skin.custom[1] && c.SkinsForTest().Rule() == SKIN_RULE_OFF,
	      "the host switching skins off puts bob in the default skin");
	rules.skins = SKIN_RULE_SYNC;
	c.HandleMessage(WrapAny(rules));
	for (const SkinChunk &piece : Pieces(skin)) {
		S_PlayerSkin p;
		InitHeader(p, 1);
		p.playerId = 1;
		p.chunk    = piece;
		c.HandleMessage(WrapSkin(p));
	}
	Check(g_skin.custom[1], "and back on, his skin is worn again when it comes");

	S_PlayerLeave leave;
	InitHeader(leave, 1);
	leave.playerId = 1;
	c.HandleMessage(WrapAny(leave));
	Check(!g_skin.custom[1] && !c.SkinsForTest().WornBy(1), "bob leaving takes his skin away");
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

bool PushesAt(const std::vector<uint8_t> &img, uint32_t site, uint32_t value) {
	return img[site - IMAGE_BASE] == 0x68 && Dword(img, site + 1) == value;
}

void TestAgainstTheImage() {
	std::printf("\nthe player's skin against gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the skin "
		            "against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// RenderPlayerCB.
	Check(Bytes(img, CVisibilityPlugins__RenderPlayerCB, {0xA1}) &&
	          Dword(img, CVisibilityPlugins__RenderPlayerCB + 1) == PLAYER_SKIN_TEXTURE &&
	          PLAYER_SKIN_TEXTURE == 0x00941428,
	      "RenderPlayerCB opens by reading Players[0].m_pSkinTexture");
	Check(Bytes(img, 0x00528B38, {0x8B, 0x5C, 0x24, 0x08}) &&
	          Bytes(img, 0x00528B3E, {0x50, 0x8B, 0x43, 0x18}) && PushesAt(img, 0x00528B42, 0x00528B10) &&
	          CallsAt(img, 0x00528B48, 0x005ACBF0),
	      "and lays it over the atomic's geometry's materials");
	Check(Bytes(img, 0x00528B50, {0x53}) && CallsAt(img, 0x00528B51, 0x0059E690) &&
	          Bytes(img, 0x00528B56, {0x89, 0xD8, 0x59, 0x5B, 0xC3}),
	      "then draws it the default way and returns the atomic, reading nothing else");
	Check(Bytes(img, 0x00528B10, {0x8B, 0x44, 0x24, 0x08}) && CallsAt(img, 0x00528B1B, 0x005ADD10),
	      "SetTextureCB is RpMaterialSetTexture");

	// Who gets it.
	Check(CallsAt(img, 0x0051021A, 0x004F8830) &&
	          Bytes(img, 0x00510238, {0xB9, 0x07, 0x00, 0x00, 0x00, 0x8D, 0x73, 0x04}) &&
	          Bytes(img, 0x00510240, {0xBF, 0x08, 0xE9, 0x5F, 0x00, 0xF3, 0xA6}) &&
	          Bytes(img, 0x005FE908, {'p', 'l', 'a', 'y', 'e', 'r', 0}) &&
	          PushesAt(img, PED_SETCLUMP_PLAYER_RENDERER, CVisibilityPlugins__RenderPlayerCB) &&
	          PushesAt(img, PED_SETCLUMP_PLAYER_RENDERER + 5, 0x004F8940) &&
	          CallsAt(img, PED_SETCLUMP_PLAYER_RENDERER + 11, RpClumpForAllAtomics),
	      "CPedModelInfo::SetClump gives it to every atomic of a model named \"player\"");
	Check(Bytes(img, 0x00528C20, {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04}) &&
	          Bytes(img, 0x00528C31, {0x89, 0x41, uint8_t(offs::RPATOMIC_RENDER_CB)}) &&
	          CallsAt(img, 0x004F894B, 0x00528C20),
	      "an atomic's render callback is the field at +48h");
	Check(Bytes(img, 0x00528B69, {0x8B, 0x6B, uint8_t(offs::RPATOMIC_CLUMP)}),
	      "and its clump the one at +3Ch");
	Check(Bytes(img, RpClumpForAllAtomics, {0x8B, 0x44, 0x24, 0x04, 0x53, 0x55, 0x56, 0x57}) &&
	          Bytes(img, 0x0059EDD8, {0x8D, 0x78, 0x08}) &&
	          Bytes(img, 0x0059EDEC, {0x83, 0xC0, 0xC0}) && Bytes(img, 0x0059EDF1, {0xFF, 0xD5}),
	      "RpClumpForAllAtomics walks the clump's atomics, calling back with each");

	// The texture's owner and the default skin.
	Check(Bytes(img, 0x004A1703, {0x8B, 0x83, 0x38, 0x01, 0x00, 0x00}) &&
	          CallsAt(img, 0x004A170E, RwTextureDestroy) &&
	          Bytes(img, 0x004A171E, {0x8D, 0x83, 0x18, 0x01, 0x00, 0x00}) &&
	          CallsAt(img, 0x004A1725, CPlayerSkin__GetSkinTexture) &&
	          PLAYER_SKIN_NAME == CWorld__Players + 0x118,
	      "LoadPlayerSkin drops m_pSkinTexture (+138h) and loads m_aSkinName's (+118h)");
	Check(PushesAt(img, 0x004A173A, PLAYER_SKIN_DEFAULT_NAME) &&
	          CallsAt(img, 0x004A173F, CPlayerSkin__GetSkinTexture) &&
	          Bytes(img, PLAYER_SKIN_DEFAULT_NAME, {'$', '$', '"', '"', 0}),
	      "and falls back on the default skin by its name, $$\"\"");
	Check(Bytes(img, CPlayerSkin__GetSkinTexture, {0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x10}) &&
	          CallsAt(img, 0x0059BA0F, 0x005A7580),
	      "GetSkinTexture looks in the skin dictionary first");

	// RenderWare.
	Check(CallsAt(img, 0x0059BA8F, RwImageFindRasterFormat) &&
	          Bytes(img, 0x0059BA8C, {0x6A, uint8_t(rwRASTERTYPETEXTURE)}) &&
	          CallsAt(img, 0x0059BAAB, RwRasterCreate) &&
	          CallsAt(img, 0x0059BAB7, RwRasterSetFromImage) &&
	          CallsAt(img, 0x0059BABF, RwTextureCreate) && CallsAt(img, 0x0059BB06, RwImageDestroy),
	      "GetSkinTexture turns an image into a texture with the five calls the engine half "
	      "makes");
	Check(CallsAt(img, 0x005B0005, RwImageCreate) && CallsAt(img, 0x005B001C, RwImageAllocatePixels),
	      "RtBMPImageRead makes its image with RwImageCreate and RwImageAllocatePixels");
	Check(Bytes(img, 0x005A9211, {0x89, 0x46, uint8_t(offs::RWIMAGE_STRIDE)}) &&
	          Bytes(img, 0x005A9226, {0x89, 0x46, uint8_t(offs::RWIMAGE_PIXELS)}),
	      "which puts the stride at +10h and the pixels at +14h");
	Check(Bytes(img, RwTextureDestroy, {0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0x56, 0x54, 0x4A}),
	      "RwTextureDestroy drops one reference");
}

} // namespace

int RunSkinTests() {
	g_skinFailures = 0;
	TestTheWire();
	TestPuttingItBackTogether();
	TestTheBmpReader();
	TestOursGoesOut();
	TestTheirsComeIn();
	TestThroughTheClient();
	TestAgainstTheImage();
	return g_skinFailures;
}
