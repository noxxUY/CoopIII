// Skipping a cutscene together, the client's half: what the skip input does,
// whose scene it is, the view the game half reads, the count's line and where
// it goes, the packets through the client - and, with a copy of the retail
// exe, the bytes game/cutsceneskip.cpp redirects and the gate it reads.

#include "chatfeed.h"
#include "client.h"
#include "cutsceneskipview.h"
#include "game/addresses.h"
#include "game/fontcull.h"
#include "game/leadcheck.h"

#include <coopiii/protocol.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace coopiii;
using namespace coopiii::game;

namespace {

int g_skipFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_skipFailures;
}

template <class T>
Message Wrap(const T &pkt) {
	Message m;
	m.opcode  = T::OPCODE;
	m.channel = CH_EVENT;
	m.data.resize(sizeof(T));
	std::memcpy(m.data.data(), &pkt, sizeof(T));
	return m;
}

CutsceneVoteBody Count(uint8_t voteId, const CutsceneKey &key, uint8_t yes, uint8_t voters) {
	CutsceneVoteBody b{};
	b.voteId = voteId;
	b.yes    = yes;
	b.voters = voters;
	b.needed = static_cast<uint8_t>((voters * 3u + 3u) / 4u);
	b.key    = key;
	return b;
}

void TestTheSkipInput() {
	std::printf("\nwhat the skip input does\n");
	using P = SkipPress;
	Check(DecideSkipPress(false, CUTSCENE_SCOPE_OWN, false, false, false) == P::Local,
	      "no session: skips, as in single player");
	Check(DecideSkipPress(false, CUTSCENE_SCOPE_SHARED, true, true, false) == P::Local,
	      "no session, whatever the view still says: skips");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_OWN, false, false, false) == P::Local,
	      "alone in the intro: skips");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_OWN, false, true, false) == P::Vote,
	      "somebody else in the intro: a vote");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_OWN, false, true, true) == P::Nothing,
	      "and a second press is nothing");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_SHARED, false, false, false) == P::Local,
	      "the mission's owner alone in its scene: skips, and the mission moves on");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_SHARED, false, true, false) == P::Vote,
	      "the owner with a participant in it: a vote");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_SHARED, true, true, false) == P::Vote,
	      "a participant with the owner in it: a vote");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_SHARED, true, false, false) == P::Nothing,
	      "a participant alone in the count: nothing, the scene is the owner's");
	Check(DecideSkipPress(true, CUTSCENE_SCOPE_NONE, false, false, false) == P::Local,
	      "a skip the game allowed in a scene we never reported: the game's");
}

void TestWhoseScene() {
	std::printf("\nwhose scene it is\n");
	Check(CutsceneScopeFor(true, false, true, false) == CUTSCENE_SCOPE_SHARED,
	      "the session's mission, on its owner");
	Check(CutsceneScopeFor(true, false, false, true) == CUTSCENE_SCOPE_SHARED,
	      "and on a participant replaying it");
	Check(CutsceneScopeFor(true, true, false, true) == CUTSCENE_SCOPE_OWN,
	      "a participant busy with a mission of its own: its own");
	Check(CutsceneScopeFor(true, false, false, false) == CUTSCENE_SCOPE_OWN,
	      "somebody not in the running mission: their own");
	Check(CutsceneScopeFor(false, false, false, false) == CUTSCENE_SCOPE_OWN,
	      "no session mission, a new game's intro: its own");

	const CutsceneKey k = MakeCutsceneKey(CUTSCENE_SCOPE_OWN, "BET");
	Check(k.scope == CUTSCENE_SCOPE_OWN && std::strcmp(k.name, "bet") == 0 && k.name[3] == '\0' &&
	          k.name[7] == '\0',
	      "the name goes lower case, zeros after it");
	const CutsceneKey l = MakeCutsceneKey(CUTSCENE_SCOPE_SHARED, "ABCDEFGHIJ");
	Check(std::memcmp(l.name, "abcdefgh", 8) == 0, "eight bytes at most, as the engine keeps it");
	Check(IsCreditsCutscene("end") && IsCreditsCutscene("END") && !IsCreditsCutscene("end2") &&
	          !IsCreditsCutscene("en") && !IsCreditsCutscene("bet"),
	      "the credits are 'end' and only 'end', as faststricmp says");
	char name[CUTSCENE_NAME_LEN + 1];
	CutsceneKey odd = k;
	odd.name[1]     = '\x01';
	CutsceneKeyName(odd, name);
	Check(std::strcmp(name, "b?t") == 0, "a name for the log is printable");
	CutsceneKeyName(l, name);
	Check(std::strcmp(name, "abcdefgh") == 0, "and terminated at eight");
}

void TestTheView() {
	std::printf("\nthe view\n");
	const CutsceneKey intro = MakeCutsceneKey(CUTSCENE_SCOPE_OWN, "bet");
	const CutsceneKey joey  = MakeCutsceneKey(CUTSCENE_SCOPE_SHARED, "j1_lfl");
	CutsceneSkipView  v;
	Check(!v.In() && !v.Crowded() && !v.HaveCast(), "nothing yet");
	v.OnVote(Count(3, intro, 0, 2));
	Check(!v.Crowded(), "a count for a scene we never said we were in is dropped");
	v.Report(intro);
	v.OnVote(Count(3, intro, 0, 1));
	Check(v.In() && !v.Crowded(), "alone: not crowded");
	v.OnVote(Count(3, intro, 0, 2));
	Check(v.Crowded() && !v.HaveCast(), "two: crowded");
	v.NoteCast();
	Check(v.HaveCast() && v.castFor == 3, "said skip in count 3");
	v.OnVote(Count(3, intro, 1, 2));
	Check(v.HaveCast() && v.vote.yes == 1, "the count comes back with ours in it");
	v.OnVote(Count(4, intro, 0, 2));
	Check(!v.HaveCast(), "a new count for the same scene needs a new press");
	v.OnVote(Count(9, joey, 0, 3));
	Check(v.vote.voteId == 4, "somebody else's scene's count is not ours");
	v.Report(joey);
	Check(!v.Crowded() && !v.HaveCast(), "into another scene: the old count is gone");
	v.Report(CutsceneKey{});
	Check(!v.In(), "out of it");
	v.OnSkip(4, intro);
	Check(v.skipWaiting && v.skipVoteId == 4, "a skip waits for the game half");
}

void TestTheLine() {
	std::printf("\nthe count's line\n");
	char out[48];
	FormatSkipCounter(out, sizeof out, 0, 2, false);
	Check(std::strcmp(out, "Skip 0/2 - press Enter") == 0, "before we press: the count and the key");
	FormatSkipCounter(out, sizeof out, 1, 2, true);
	Check(std::strcmp(out, "Skip 1/2") == 0, "after: the count alone");
	FormatSkipCounter(out, sizeof out, 5, 8, false);
	bool drawable = true;
	for (const char *c = out; *c; ++c)
		drawable = drawable && FeedGlyph(static_cast<unsigned char>(*c), c == out) == *c;
	Check(drawable, "every character is one CFont draws as itself");
	char tiny[6];
	FormatSkipCounter(tiny, sizeof tiny, 1, 2, true);
	Check(std::strlen(tiny) == 5, "cut to the buffer, and terminated");
}

void TestWhereItGoes() {
	std::printf("\nwhere the count goes\n");
	// A wide window: bottom right, level with the version mark.
	float            cull = TextCullLine(1920.0f, 1080.0f, false);
	const MarkLayout v    = MeasureVersionMark(1080.0f, cull);
	const MarkLayout c    = MeasureCornerMark(1920.0f, 1080.0f, cull, 200.0f);
	const float      unit = 1080.0f / 448.0f;
	Check(c.y == v.y && c.scaleX == v.scaleX && c.scaleY == v.scaleY,
	      "1920x1080: the version mark's height and size");
	Check(c.x + 200.0f > 1920.0f - MARK_MARGIN_UNITS * unit - 0.01f &&
	          c.x + 200.0f < 1920.0f - MARK_MARGIN_UNITS * unit + 0.01f,
	      "and its margin, from the right edge");
	Check(FEED_CELL_HEIGHT * c.scaleY >= MARK_MIN_TEXT_PX - 0.01f,
	      "never smaller than the version mark's floor");

	// A small window: the floor.
	cull                = TextCullLine(640.0f, 360.0f, false);
	const MarkLayout sm = MeasureCornerMark(640.0f, 360.0f, cull, 100.0f);
	Check(FEED_CELL_HEIGHT * sm.scaleY >= MARK_MIN_TEXT_PX - 0.01f && sm.y > 300.0f,
	      "640x360: 14 px at least, still at the bottom");

	// Taller than wide, retail's y test: kept above the line PrintChar drops at.
	cull                  = TextCullLine(958.0f, 1000.0f, false);
	const MarkLayout tall = MeasureCornerMark(958.0f, 1000.0f, cull, 150.0f);
	Check(tall.y <= cull - MARK_CULL_CLEARANCE_PX + 0.01f && tall.y > 900.0f,
	      "958x1000: up to the cull line and no further");
	Check(tall.x + 150.0f <= 958.0f, "and still on the screen");

	const MarkLayout wide = MeasureCornerMark(300.0f, 1080.0f, TextCullLine(300.0f, 1080.0f, false),
	                                          900.0f);
	Check(wide.x >= MARK_MARGIN_UNITS * (1080.0f / 448.0f) - 0.01f,
	      "a line wider than the screen starts at the left margin rather than off it");
}

void TestTheClientRoutesIt() {
	std::printf("\nthe count through the client\n");
	Client c;
	S_Welcome w{};
	InitHeader(w, 1000);
	w.playerId     = 1;
	w.netId        = 101;
	w.maxPlayers   = MAX_PLAYERS;
	w.snapshotHz   = SNAPSHOT_HZ;
	w.hostPlayerId = INVALID_PLAYER;
	c.HandleMessage(Wrap(w));

	const CutsceneKey intro = MakeCutsceneKey(CUTSCENE_SCOPE_OWN, "bet");
	Check(!c.CastCutsceneSkip(), "in no scene: no press goes out");
	c.ReportCutscene(intro);
	Check(SameCutscene(c.SkipView().reported, intro), "the scene is what we said");

	S_CutsceneVote sv{};
	InitHeader(sv, 1000);
	sv.kind = CUTSCENE_VOTE_COUNT;
	sv.body = Count(7, intro, 0, 1);
	c.HandleMessage(Wrap(sv));
	Check(!c.SkipView().Crowded() && !c.CastCutsceneSkip(), "alone: the press is the game's");

	sv.body = Count(7, intro, 0, 2);
	c.HandleMessage(Wrap(sv));
	Check(c.SkipView().Crowded(), "two in it: the count reaches the view");
	Check(c.CastCutsceneSkip(), "our press goes out");
	Check(!c.CastCutsceneSkip(), "once");

	CutsceneKey key{};
	uint8_t     vote = 0;
	Check(!c.TakeCutsceneSkip(key, vote), "no skip until one is ordered");
	sv.kind = CUTSCENE_VOTE_SKIP;
	sv.body = Count(7, intro, 2, 2);
	c.HandleMessage(Wrap(sv));
	Check(c.TakeCutsceneSkip(key, vote) && vote == 7 && SameCutscene(key, intro),
	      "the skip is handed over");
	Check(!c.TakeCutsceneSkip(key, vote), "once");

	c.HandleMessage(Wrap(w));
	Check(!c.SkipView().In() && !c.SkipView().haveVote,
	      "a new session forgets the scene, so it is said again");
}

// ---- the bytes in the real exe ----------------------------------------------------------

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

// File offset = VA - image base in this image, as fontcull's test reads it.
const uint8_t *At(const std::vector<uint8_t> &img, uintptr_t va) { return &img[va - IMAGE_BASE]; }

uint32_t Dword(const std::vector<uint8_t> &img, uintptr_t va) {
	uint32_t v;
	std::memcpy(&v, At(img, va), 4);
	return v;
}

bool BytesAt(const std::vector<uint8_t> &img, uintptr_t va, std::initializer_list<uint8_t> bytes) {
	size_t i = 0;
	for (uint8_t b : bytes)
		if (At(img, va)[i++] != b)
			return false;
	return true;
}

constexpr uint32_t TEXT_BEGIN = 0x00401000;
constexpr uint32_t TEXT_END   = 0x005E3238;

// Every rel32 call, jmp and jcc in .text whose target lands strictly inside
// [site, site + 5): a redirect of the five bytes would cut through it. The
// short jumps near each site were read in the disassembly (addresses.h);
// read byte by byte here they would find branches in the middle of other
// instructions.
int BranchesInto(const std::vector<uint8_t> &img, uintptr_t site) {
	int n = 0;
	for (uint32_t va = TEXT_BEGIN; va + 6 < TEXT_END; ++va) {
		const uint8_t *p   = At(img, va);
		int64_t        dst = -1;
		if (p[0] == 0xE8 || p[0] == 0xE9) {
			int32_t rel;
			std::memcpy(&rel, p + 1, 4);
			dst = int64_t(va) + 5 + rel;
		} else if (p[0] == 0x0F && p[1] >= 0x80 && p[1] <= 0x8F) {
			int32_t rel;
			std::memcpy(&rel, p + 2, 4);
			dst = int64_t(va) + 6 + rel;
		}
		if (dst > int64_t(site) && dst < int64_t(site) + 5)
			++n;
	}
	return n;
}

void TestTheBytesInTheImage() {
	std::printf("\nthe skip in gta3.exe\n");
	std::vector<uint8_t> img;
	std::string          from;
	if (!LoadExe(img, from)) {
		std::printf("  [skipped] no retail gta3.exe; set COOPIII_GTA3_EXE to check the "
		            "bytes against one\n");
		return;
	}
	std::printf("  reading %s\n", from.c_str());

	// The call that is taken, and that it is the only way to FinishCutscene.
	Check(RelCallAt(At(img, CCutsceneMgr__Update_FinishCall), CCutsceneMgr__Update_FinishCall,
	                CCutsceneMgr__FinishCutscene),
	      "0x00405131 is `call 0x00405140`");
	int calls = 0, dwords = 0;
	for (uint32_t va = TEXT_BEGIN; va + 5 < TEXT_END; ++va) {
		const uint8_t *p = At(img, va);
		if ((p[0] == 0xE8 || p[0] == 0xE9) && RelCallAt(p, va, CCutsceneMgr__FinishCutscene))
			++calls;
		if ((p[0] == 0xE9) && va + 5 + int32_t(Dword(img, va + 1)) == CCutsceneMgr__FinishCutscene)
			++calls;
	}
	for (size_t o = 0; o + 4 <= img.size(); ++o) {
		uint32_t v;
		std::memcpy(&v, &img[o], 4);
		if (v == CCutsceneMgr__FinishCutscene)
			++dwords;
	}
	Check(calls == 1 && dwords == 0,
	      "and the only call or jump to FinishCutscene in .text, with no pointer to it anywhere");
	Check(BytesAt(img, CCutsceneMgr__FinishCutscene, {0xB9, 0xF8, 0xAC, 0x6F, 0x00, 0x83, 0xEC, 0x08}) &&
	          BytesAt(img, 0x0040519F, {0x83, 0xC4, 0x08, 0xC3}),
	      "FinishCutscene: `mov ecx,TheCamera / sub esp,8` ... `add esp,8 / ret`, cdecl, no arguments");
	Check(BranchesInto(img, CCutsceneMgr__Update_FinishCall) == 0,
	      "nothing branches into the middle of the five bytes");

	// The gate the game half reads, in Update's own words.
	Check(BytesAt(img, 0x00404F8C, {0x80, 0x3D, 0xF5, 0xCC, 0x95, 0x00, 0x00}) &&
	          Dword(img, 0x00404F8E) == CCutsceneMgr__ms_started,
	      "Update's first test past the load switch is ms_running at 0x0095CCF5");
	Check(BytesAt(img, 0x00404EBD, {0xC6, 0x05}) && Dword(img, 0x00404EBF) == CCutsceneMgr__ms_started &&
	          img[0x00404EC3 - IMAGE_BASE] == 1,
	      "which SetupCutsceneToStart turns on");
	Check(Dword(img, 0x00404FA6) == CCutsceneMgr__ms_cutsceneName &&
	          Dword(img, 0x00404FAB) == CUTSCENE_NAME_END &&
	          std::memcmp(At(img, CUTSCENE_NAME_END), "end", 4) == 0,
	      "the scene's name against \"end\"");
	Check(BytesAt(img, 0x0040501C, {0x0F, 0xB6, 0x05}) && Dword(img, 0x0040501F) == TheCamera__ActiveCam &&
	          BytesAt(img, 0x00405023, {0x6B, 0xC0, 0x69}) &&
	          BytesAt(img, 0x00405026, {0x66, 0x83, 0x3C, 0x85}) &&
	          Dword(img, 0x0040502A) == TheCamera__Cams_Mode &&
	          img[0x0040502E - IMAGE_BASE] == uint8_t(CAM_MODE_FLYBY) && 0x69 * 4 == CAMERA_CAM_STRIDE,
	      "Cams[ActiveCam].Mode == MODE_FLYBY, 0x1A4 a camera");
	Check(TheCamera__ActiveCam == TheCamera + 0x76 && TheCamera__Cams_Mode == TheCamera + 0x1B0,
	      "both inside TheCamera");
	Check(BytesAt(img, 0x00405035, {0x83, 0x3D}) &&
	          Dword(img, 0x00405037) == CCutsceneMgr__ms_cutsceneLoadStatus && img[0x0040503B - IMAGE_BASE] == 0,
	      "the load settled");
	Check(Dword(img, 0x0040506D) == CGame__playingIntro && Dword(img, 0x005898CA) == CGame__playingIntro &&
	          Dword(img, 0x005898DD) == CGame__playingIntro,
	      "Start only in the intro, the flag SET_INTRO_IS_PLAYING writes");

	// The intro's own button test.
	Check(Dword(img, 0x005EEC40 + (0xE1 - 0xD6) * 4) == IS_BUTTON_PRESSED_HANDLER,
	      "IS_BUTTON_PRESSED is 0x0043DAF9 in the 200 table");
	Check(RelCallAt(At(img, IS_BUTTON_PRESSED_CompareCall), IS_BUTTON_PRESSED_CompareCall,
	                CRunningScript__UpdateCompareFlag) &&
	          BytesAt(img, 0x0043DBE7, {0x89, 0xD9, 0xFF, 0x74, 0x24, 0x04}),
	      "its answer goes `mov ecx,ebx / push [esp+4] / call UpdateCompareFlag`");
	Check(BytesAt(img, 0x0044FE0B, {0xC2, 0x04, 0x00}), "which returns `ret 4`");
	Check(Dword(img, 0x0043DB2F) == CGame__playingIntro && Dword(img, 0x0043DB3C) == CTheScripts__ScriptParams &&
	          Dword(img, 0x0043DB49) == CTheScripts__ScriptParams + 4 &&
	          img[0x0043DB4D - IMAGE_BASE] == uint8_t(PAD_BUTTON_START),
	      "the intro's extra inputs go with pad 0, button 12, off ScriptParams");
	const uint32_t table = 0x005F0014;
	Check(BytesAt(img, Dword(img, table + PAD_BUTTON_START * 4), {0x66, 0x8B, 0x40, 0x18}) &&
	          BytesAt(img, Dword(img, table + PAD_BUTTON_CROSS * 4), {0x66, 0x8B, 0x40, 0x20}),
	      "GetPadState's button 12 is Start (+18h) and 16 is Cross (+20h)");
	Check(BranchesInto(img, IS_BUTTON_PRESSED_CompareCall) == 0,
	      "nothing branches into the middle of those five bytes either");
}

} // namespace

int RunCutsceneSkipTests() {
	TestTheSkipInput();
	TestWhoseScene();
	TestTheView();
	TestTheLine();
	TestWhereItGoes();
	TestTheClientRoutesIt();
	TestTheBytesInTheImage();
	return g_skipFailures;
}
