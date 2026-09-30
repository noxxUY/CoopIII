// The progress file's half that needs no network: server/core/progress.h, the
// campaign log picked back up, and which hidden packages go in the file.

#include "progress.h"
#include "session.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace coopiii;

namespace {

int g_progressFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_progressFailures;
}

Player *JoinAs(Session &s, uint32_t peer, const char *nick) {
	RejectReason reject = REJECT_NONE;
	return s.AddPlayer(peer, nick, 7, PROTOCOL_VERSION, reject);
}

PickupIdent Package(float x) {
	PickupIdent id{};
	id.pos        = {x, 50.0f, 10.0f};
	id.modelIndex = 1321;
	id.type       = PICKUP_TYPE_PACKAGE;
	return id;
}

CampaignDeltaBody Delta(uint16_t mission, uint16_t offset, int32_t value, bool last) {
	CampaignDeltaBody b{};
	b.missionNumber    = mission;
	b.valueCount       = 1;
	b.values[0].offset = offset;
	b.values[0].value  = value;
	b.last             = last ? 1 : 0;
	b.scriptHash       = 0xC0FFEE11u;
	return b;
}

SavedProgress Sample() {
	CampaignLog log;
	log.Append(0, Delta(19, 900, 1, false));
	log.Append(0, Delta(19, 904, 7, true));
	log.Append(2, Delta(20, 908, 1, true));
	SavedProgress p;
	p.logId       = log.Id();
	p.deltas      = log.Entries();
	p.carLists[0] = 0x5;
	p.carLists[3] = 0x40;
	p.packages    = {Package(1.0f), Package(2.0f)};
	return p;
}

void TestTheFileRoundTrips() {
	std::printf("\nthe progress file, there and back\n");
	const SavedProgress p     = Sample();
	const std::string   bytes = EncodeProgress(p);
	SavedProgress       back;
	Check(DecodeProgress(bytes, &back), "what is written reads back");
	bool same = back.logId == p.logId && back.deltas.size() == 3 && back.packages.size() == 2 &&
	            back.carLists[0] == 0x5 && back.carLists[3] == 0x40;
	for (size_t i = 0; same && i < back.deltas.size(); ++i)
		same = back.deltas[i].body.seq == i + 1 &&
		       back.deltas[i].body.values[0].offset == p.deltas[i].body.values[0].offset &&
		       back.deltas[i].body.scriptHash == 0xC0FFEE11u;
	Check(same, "with the log's name, every delta in order, the lists and the packages");

	std::string flipped = bytes;
	flipped[sizeof(uint32_t) * 12] ^= 0x10;
	Check(!DecodeProgress(flipped, &back), "a byte gone wrong is caught by the checksum");
	Check(!DecodeProgress(bytes.substr(0, bytes.size() - 5), &back), "and a file cut short is refused");
	std::string other = bytes;
	other[7]          = '2';
	Check(!DecodeProgress(other, &back), "and so is another format's");
	Check(!DecodeProgress(std::string(), &back), "and an empty one");

	SavedProgress empty;
	empty.logId = 9;
	Check(DecodeProgress(EncodeProgress(empty), &back) && back.logId == 9 && back.deltas.empty() &&
	          back.packages.empty(),
	      "a session that did nothing yet still has a file that reads");
}

void TestTheLogIsPickedUpWhereItWas() {
	std::printf("\nthe campaign log, picked up where it was\n");
	const SavedProgress p = Sample();
	CampaignLog         log;
	Check(log.Restore(p.logId, p.deltas), "a log kept from last time is taken");
	Check(log.Id() == p.logId, "under the same name, so a client that kept its copy keeps it");
	Check(log.Count() == 3 && log.Last() == 3, "with the same numbers");
	const std::vector<S_CampaignDelta> since = log.Since(1);
	Check(since.size() == 2 && since[0].body.seq == 2 && since[1].body.seq == 3,
	      "and a player who had the first is sent the other two");
	Check(since.size() == 2 && since[0].ownerId == INVALID_PLAYER &&
	          since[1].ownerId == INVALID_PLAYER,
	      "owned by nobody, since last time's slots mean nothing now");
	Check(log.Append(1, Delta(21, 912, 1, true)) == 4, "and the next mission is number 4");

	std::vector<CampaignLog::Entry> gap = p.deltas;
	gap.erase(gap.begin() + 1);
	CampaignLog other;
	const uint32_t before = other.Id();
	Check(!other.Restore(p.logId, gap), "a log with a number missing is refused");
	Check(other.Id() == before && other.Count() == 0, "and leaves the log as it was");
	Check(!other.Restore(0, p.deltas), "and so is one named 0, which is nobody's");
}

void TestWhichPackagesAreKept() {
	std::printf("\nwhich hidden packages go in the file\n");
	Session s;
	Player *a = JoinAs(s, 1, "alice");
	Player *b = JoinAs(s, 2, "bob");
	const PickupIdent one = Package(1.0f), two = Package(2.0f);
	s.ClaimPickup(a->id, one, 10);
	Check(s.KeptPackages().empty(), "one somebody is only standing near is not");
	s.NotePickupCollected(a->id, one, 20);
	Check(s.KeptPackages().size() == 1, "one somebody took is");
	PickupIdent uzi = Package(3.0f);
	uzi.type        = 2;
	s.ClaimPickup(a->id, uzi, 30);
	s.NotePickupCollected(a->id, uzi, 40);
	Check(s.KeptPackages().size() == 1, "and a gun is not a package");

	s.SetPackageRule(PACKAGES_PERPLAYER);
	s.ClaimPickup(b->id, two, 50);
	s.NotePickupCollected(b->id, two, 60);
	Check(s.KeptPackages().size() == 0,
	      "under perplayer nobody's is, since the slot that found it means nothing next time");
	s.SetPackageRule(PACKAGES_SHARED);

	// The next sitting.
	Session next;
	next.RestorePackages({one, two});
	next.RestorePackages({one});
	Check(next.TakenPickups().size() == 2, "last time's are put back, once each");
	Player *c = JoinAs(next, 3, "carol");
	Check(next.ClaimPickup(c->id, one, 10) == Session::PickupVerdict::DENIED,
	      "and nobody can take one again");
	Check(next.BuildBackfill(c->id, 20).pickups.size() == 2,
	      "and a joiner is told they are gone, so his game counts them");
	Check(next.KeptPackages().size() == 2, "and they go in the file again");
	next.ExpirePickups(0x7FFFFFFF);
	Check(next.TakenPickups().size() == 2, "they never run out");

	Session perPlayer;
	perPlayer.SetPackageRule(PACKAGES_PERPLAYER);
	perPlayer.RestorePackages({one});
	Player *d = JoinAs(perPlayer, 4, "dave");
	Check(perPlayer.ClaimPickup(d->id, one, 10) == Session::PickupVerdict::GRANTED &&
	          perPlayer.BuildBackfill(d->id, 20).pickups.empty(),
	      "under perplayer they are nobody's and in nobody's way");
	Check(perPlayer.KeptPackages().size() == 1, "but they are still kept for a shared session");

	next.ForgetPackages();
	Check(next.TakenPickups().empty() && next.KeptPackages().empty(),
	      "and forgetting the progress forgets them");
}

bool Has(const std::vector<PickupIdent> &set, const PickupIdent &p) {
	for (const PickupIdent &q : set)
		if (Session::SameIdent(q, p))
			return true;
	return false;
}

bool FoundBy(const Session::PackageNews &n, const PickupIdent &p, uint8_t by) {
	for (const Session::PackageNews::Found &f : n.found)
		if (f.byPlayerId == by && Session::SameIdent(f.ident, p))
			return true;
	return false;
}

void TestTwoSavesMakeOneList() {
	std::printf("\nthe packages two saves found before the session count for both\n");
	constexpr uint32_t kHash = 0xC0FFEE11u;
	const PickupIdent p1 = Package(1.0f), p2 = Package(2.0f), p3 = Package(3.0f),
	                  p4 = Package(4.0f);

	Session s;
	Player *host  = JoinAs(s, 1, "host");
	Player *guest = JoinAs(s, 2, "guest");

	// The host's save has found p3; the guest's, a newer one, p1 and p2.
	Session::PackageNews n;
	s.NotePackagesLive(host->id, kHash, {p1, p2, p4}, &n);
	Check(n.found.empty() && n.tellReporter.empty(),
	      "one save alone says nothing: nobody knows yet what else there was");

	n = {};
	s.NotePackagesLive(guest->id, kHash, {p3, p4}, &n);
	Check(n.found.size() == 3 && FoundBy(n, p1, guest->id) && FoundBy(n, p2, guest->id) &&
	          FoundBy(n, p3, host->id),
	      "the second works out what each save has that the other's world still shows");
	Check(s.KeptPackages().size() == 3, "and all three go in the progress file");
	Check(s.ClaimPickup(host->id, p1, 10) == Session::PickupVerdict::DENIED &&
	          s.ClaimPickup(guest->id, p3, 10) == Session::PickupVerdict::DENIED,
	      "and nobody can collect one of them again");

	// Both games have taken the others' away and report again.
	n = {};
	s.NotePackagesLive(host->id, kHash, {p4}, &n);
	Session::PackageNews m;
	s.NotePackagesLive(guest->id, kHash, {p4}, &m);
	Check(n.found.empty() && m.found.empty() && n.tellReporter.empty() && m.tellReporter.empty(),
	      "and once they agree, nothing is found twice");

	// The guest loads an older save with p1 and p3 still lying in it.
	n = {};
	s.NotePackagesLive(guest->id, kHash, {p1, p3, p4}, &n);
	Check(n.found.empty() && n.tellReporter.size() == 2 && Has(n.tellReporter, p1) &&
	          Has(n.tellReporter, p3),
	      "an older save loaded mid-session is told again what the session has");

	// A leaver's report goes, and what he showed stays known.
	s.RemovePeer(2);
	Player *late = JoinAs(s, 3, "late");
	n = {};
	s.NotePackagesLive(late->id, kHash, {p1, p2, p3}, &n);
	Check(FoundBy(n, p4, late->id) && n.found.size() == 1,
	      "a joiner's save is held against what anybody showed before, gone or not");
	Check(n.tellReporter.size() == 3, "and told the three the session already has");

	// Another main.scm puts its packages elsewhere.
	n = {};
	s.NotePackagesLive(host->id, 0x1234u, {Package(9.0f)}, &n);
	Check(n.found.empty(), "reports from another main.scm are never compared with these");

	// A reservation on one somebody's save already has is a collection.
	Session r;
	Player *x = JoinAs(r, 1, "x");
	Player *y = JoinAs(r, 2, "y");
	r.NotePackagesLive(x->id, kHash, {p1, p2}, &n);
	r.ClaimPickup(x->id, p1, 10);
	n = {};
	r.NotePackagesLive(y->id, kHash, {p2}, &n);
	Check(FoundBy(n, p1, y->id), "a package held by one player that the other's save has is found");
	Check(!r.NotePickupCollected(x->id, p1, 20),
	      "and the holder's collection of it afterwards changes nothing");

	// Under perplayer every save's packages are its own.
	Session pp;
	pp.SetPackageRule(PACKAGES_PERPLAYER);
	Player *e = JoinAs(pp, 1, "e");
	Player *f = JoinAs(pp, 2, "f");
	n = {};
	pp.NotePackagesLive(e->id, kHash, {p1, p2}, &n);
	pp.NotePackagesLive(f->id, kHash, {p2}, &n);
	Check(n.found.empty() && n.tellReporter.empty() && pp.TakenPickups().empty(),
	      "under perplayer a report changes nothing");
}

} // namespace

int RunProgressTests() {
	TestTheFileRoundTrips();
	TestTheLogIsPickedUpWhereItWas();
	TestWhichPackagesAreKept();
	TestTwoSavesMakeOneList();
	return g_progressFailures;
}
