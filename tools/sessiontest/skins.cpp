// The players' custom skins, the server's half: server/core/skinrelay.h. What
// is kept, who is owed it, how fast it goes, and what is never sent on.

#include "skinrelay.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace coopiii;

namespace {

int g_skinFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_skinFailures;
}

Skin MakeSkin(uint32_t side, uint32_t serial, uint32_t colours) {
	std::vector<uint8_t> rgba(size_t(side) * side * 4);
	for (size_t i = 0; i < size_t(side) * side; ++i) {
		rgba[i * 4]     = uint8_t(i % colours);
		rgba[i * 4 + 1] = uint8_t((i % colours) >> 8);
		rgba[i * 4 + 2] = 7;
	}
	Skin s;
	EncodeSkin(side, side, rgba.data(), serial, "test", s);
	return s;
}

std::vector<C_PlayerSkin> Pieces(const Skin &skin) {
	std::vector<C_PlayerSkin> out;
	uint32_t                  at = 0;
	C_PlayerSkin              p;
	InitHeader(p, 1);
	while (CutSkinPiece(skin, at, p.chunk))
		out.push_back(p);
	return out;
}

bool Feed(SkinRelay &r, uint8_t from, const Skin &skin) {
	bool whole = false;
	for (const C_PlayerSkin &p : Pieces(skin))
		whole = r.Take(from, p) || whole;
	return whole;
}

// Everything `to` is owed at `now`, as a receiver puts it together.
size_t Drain(SkinRelay &r, uint8_t to, uint32_t now, std::vector<S_PlayerSkin> &out) {
	size_t       n = 0;
	S_PlayerSkin p;
	while (r.Next(to, now, p)) {
		out.push_back(p);
		++n;
	}
	return n;
}

void TestTheRelay() {
	std::printf("custom skins on the server\n");
	SkinRelay r;
	r.Joined(0);
	r.Joined(1);
	const Skin big = MakeSkin(256, 1, 600);   // RGB, 192 pieces
	const std::vector<C_PlayerSkin> pieces = Pieces(big);
	Check(pieces.size() == 192, "(a 256x256 RGB skin is 192 pieces)");

	bool early = false;
	for (size_t i = 0; i + 1 < pieces.size(); ++i)
		early = r.Take(0, pieces[i]) || early;
	S_PlayerSkin p;
	Check(!early && !r.Held(0) && !r.Next(1, 0, p),
	      "nothing of a skin is kept or sent on until all of it is in");
	Check(r.Take(0, pieces.back()) && r.Held(0) && r.Held(0)->bytes == big.info.bytes,
	      "the last piece makes it alice's");
	Check(!r.Owed(0) && r.Owed(1), "owed to bob, not back to alice");

	std::vector<S_PlayerSkin> got;
	const size_t burst = Drain(r, 1, 1000, got);
	Check(burst == SKIN_BURST_BYTES / SKIN_CHUNK_BYTES, "a burst goes at once");
	Check(Drain(r, 1, 1000, got) == 0, "and nothing more in the same instant");
	uint32_t now = 1000;
	while (r.Owed(1) && now < 20000)
		Drain(r, 1, now += 10, got);
	Check(!r.Owed(1) && got.size() == 192 && now >= 2300 && now <= 2600,
	      "the rest at 128 KiB a second: 192 KiB in about a second and a half");
	SkinAssembly in;
	SkinAssembly::Step last = SkinAssembly::Step::Partial;
	for (const S_PlayerSkin &s : got)
		last = in.Take(s.chunk);
	Check(last == SkinAssembly::Step::Complete && in.Finished().payload == big.payload &&
	          got.front().playerId == 0,
	      "and bob puts together exactly alice's skin");

	// A joiner is owed everybody's, and takes turns between them.
	r.Joined(2);
	const Skin small = MakeSkin(32, 1, 10);
	Check(Feed(r, 1, small), "bob's small skin is in");
	got.clear();
	Drain(r, 2, 5000, got);
	Check(got.size() >= 2 && got[0].playerId != got[1].playerId,
	      "carol, joining, is sent alice's and bob's turn about");
	now = 5000;
	while (r.Owed(2) && now < 30000)
		Drain(r, 2, now += 10, got);
	Check(!r.Owed(2), "and all of both in the end");

	// A changed skin starts over for whoever had it half.
	r.Joined(3);
	got.clear();
	Drain(r, 3, 40000, got);
	Check(r.Owed(3), "(dave has part of alice's)");
	const Skin next = MakeSkin(64, 2, 5);
	Feed(r, 0, next);
	got.clear();
	now = 40000;
	while (r.Owed(3) && now < 60000)
		Drain(r, 3, now += 10, got);
	SkinAssembly fresh;
	last = SkinAssembly::Step::Partial;
	for (const S_PlayerSkin &s : got)
		if (s.playerId == 0)
			last = fresh.Take(s.chunk);
	Check(!got.empty() && got[0].chunk.offset == 0 && last == SkinAssembly::Step::Complete &&
	          fresh.Finished().info.serial == 2,
	      "alice changing hers mid-way sends dave the new one from its first piece");

	// What does not fit is refused, and the half before it with it.
	const uint32_t rejected = r.Rejected();
	C_PlayerSkin   bad      = Pieces(small)[0];
	bad.chunk.info.width    = 48;
	Check(!r.Take(1, bad) && r.Rejected() == rejected + 1, "a size that is not a skin is refused");
	bad = Pieces(big)[0];
	bad.chunk.length = 9;
	Check(!r.Take(1, bad) && r.Rejected() == rejected + 2, "and a piece of the wrong length");
	Check(r.Held(1) && r.Held(1)->bytes == small.info.bytes,
	      "while what bob wore stays what he wears");
	Check(!r.Take(9, Pieces(small)[0]), "a player id past the session's is nobody");

	// Leaving, and the rule going off.
	r.Left(1);
	Check(!r.Held(1) && !r.Next(1, 90000, p), "bob leaving takes his skin, and he is owed nothing");
	got.clear();
	while (Drain(r, 2, now += 1000, got) != 0) {
	}
	for (const S_PlayerSkin &s : got)
		if (s.playerId == 1) {
			Check(false, "nobody is sent anything more of his");
			break;
		}
	Feed(r, 0, big);
	r.Forget();
	Check(!r.Held(0) && !r.Owed(2) && !r.Owed(3),
	      "switched off, every skin is forgotten and nothing is owed");
	Check(Feed(r, 0, small) && r.Owed(2),
	      "and the players are still there to be sent the next one");
}

} // namespace

int RunSkinRelayTests() {
	g_skinFailures = 0;
	TestTheRelay();
	return g_skinFailures;
}
