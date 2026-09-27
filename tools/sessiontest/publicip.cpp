// What the server makes of its public address, its router and Windows
// Firewall: server/core/reach.h and firewall.h. The asking is server/probe.cpp
// and needs the internet, a router and a firewall; what the answers mean and
// how they are put to the host does not, and that is what is here.

#include "firewall.h"
#include "reach.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace coopiii;

namespace {

int g_ipFailures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_ipFailures;
}

uint32_t Ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	return a << 24 | b << 16 | c << 8 | d;
}

bool Has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }

void TestTheReplyIsAnAddressOrNothing() {
	std::printf("\na what-is-my-IP reply\n");
	uint32_t a = 0;
	Check(ParseIpReply("203.0.113.9", &a) && a == Ip(203, 0, 113, 9), "a bare address");
	Check(ParseIpReply("203.0.113.9\n", &a) && a == Ip(203, 0, 113, 9),
	      "with the newline checkip.amazonaws.com ends it with");
	Check(ParseIpReply("  \r\n8.8.4.4 \r\n", &a) && a == Ip(8, 8, 4, 4),
	      "and whitespace either side");
	Check(ParseIpReply("255.255.255.255", &a) && ParseIpReply("0.0.0.0", &a) && a == 0,
	      "the two ends of the range");

	uint32_t kept = 0x01020304;
	Check(!ParseIpReply("", &kept) && !ParseIpReply("\n", &kept), "nothing is not an address");
	Check(!ParseIpReply("2001:db8::1", &kept), "nor is IPv6");
	Check(!ParseIpReply("<html><body>Blocked</body></html>", &kept),
	      "nor an error page or a captive portal");
	Check(!ParseIpReply("256.1.1.1", &kept) && !ParseIpReply("1.2.3.1000", &kept),
	      "a part past 255 is refused");
	Check(!ParseIpReply("1.2.3", &kept) && !ParseIpReply("1.2.3.4.5", &kept) &&
	          !ParseIpReply("1..3.4", &kept) && !ParseIpReply(".1.2.3", &kept),
	      "and so is the wrong number of parts");
	Check(!ParseIpReply("010.1.1.1", &kept), "a leading zero, which some read as octal");
	Check(!ParseIpReply("1.2.3.4 5.6.7.8", &kept) && !ParseIpReply("1.2.3.4x", &kept),
	      "and anything after the address");
	Check(kept == 0x01020304, "and a refusal leaves the old value alone");
}

void TestWhichAddressIsShown() {
	std::printf("\nwhich address the window hands out\n");
	const std::vector<LocalAddress> home = {{Ip(192, 168, 1, 24), true}};

	ReachStatus s;
	ShareView   v = ShareFor(home, s, 2001);
	Check(v.address == "192.168.1.24:2001" && !v.isPublic && v.lan.empty() &&
	          std::string(v.label) == "Address to share",
	      "the lookup off: this network's, as it always was");
	Check(Has(v.port.text, "forward UDP port 2001 on your router to 192.168.1.24"),
	      "with the forwarding advice it always had, now naming the machine");

	s.lookup = LookupState::Pending;
	v        = ShareFor(home, s, 2001);
	Check(v.address.empty() && std::string(v.label) == "Public address" &&
	          v.lan == "192.168.1.24:2001",
	      "being looked up: a public card with nothing in it yet, and this network's beside it");

	s.lookup     = LookupState::Found;
	s.publicAddr = Ip(203, 0, 113, 9);
	v            = ShareFor(home, s, 2001);
	Check(v.address == "203.0.113.9:2001" && v.isPublic && v.lan == "192.168.1.24:2001",
	      "found: the public one to hand out, and the same network's under its own name");
	Check(Has(v.note.text, "127.0.0.1:2001") && Has(v.note.text, "loop"),
	      "and the host told to use 127.0.0.1, since routers do not all loop back");

	s.publicAddr = Ip(10, 1, 2, 3);
	v            = ShareFor(home, s, 2001);
	Check(!v.isPublic && v.address == "192.168.1.24:2001",
	      "a private address from a service is not a public address");

	s            = ReachStatus{};
	s.lookup     = LookupState::Failed;
	s.lookupWhy  = "no answer from api.ipify.org";
	v            = ShareFor(home, s, 2500);
	Check(v.address == "192.168.1.24:2500" && !v.isPublic &&
	          Has(v.note.text, "Could not look up your public address") &&
	          Has(v.note.text, ":2500"),
	      "failed: said plainly, and the old advice again");

	s.port       = PortState::Opened;
	s.routerAddr = Ip(198, 51, 100, 7);
	v            = ShareFor(home, s, 2001);
	Check(v.isPublic && v.address == "198.51.100.7:2001" && !Has(v.note.text, "Could not"),
	      "and when the router has said its own address, that is the public one");

	const std::vector<LocalAddress> direct = {{Ip(203, 0, 113, 50), true}};
	v = ShareFor(direct, ReachStatus{}, 2001);
	Check(v.isPublic && v.address == "203.0.113.50:2001" && v.lan.empty() &&
	          Has(v.port.text, "no router port to open") && v.port.tone == Tone::Ok,
	      "a machine on the internet itself: its own address, and nothing to forward");

	v = ShareFor({}, ReachStatus{}, 2001);
	Check(v.address == "127.0.0.1:2001", "and with no address at all, loopback");
}

void TestWhatTheRouterLineSays() {
	std::printf("\nwhat the line about the router says\n");
	const std::vector<LocalAddress> home = {{Ip(192, 168, 1, 24), true}};
	ReachStatus                     s;

	s.port = PortState::Pending;
	Check(Has(PortAdvice(home, s, 2001).text, "Asking your router"), "while it is asked");

	s.port       = PortState::Opened;
	Advice a     = PortAdvice(home, s, 2001);
	Check(a.tone == Tone::Ok && a.text == "Port 2001 opened on your router automatically.",
	      "opened");
	s.routerAddr = Ip(100, 70, 1, 2);
	a            = PortAdvice(home, s, 2001);
	Check(a.tone == Tone::Warn && Has(a.text, "behind another network (100.70.1.2)"),
	      "opened, on a router that is itself behind a carrier: said, not celebrated");

	s            = ReachStatus{};
	s.port       = PortState::Failed;
	s.portWhy    = "no router answered over UPnP; it may have UPnP turned off";
	a            = PortAdvice(home, s, 2001);
	Check(a.tone == Tone::Warn &&
	          a.text ==
	              "Could not open the port automatically. Forward UDP 2001 to 192.168.1.24 on "
	              "your router.",
	      "failed: what to forward, and to where");
	Check(PortLogLine(home, s, 2001) ==
	          "could not open the port automatically. Forward UDP 2001 to 192.168.1.24 on your "
	          "router (no router answered over UPnP; it may have UPnP turned off)",
	      "and the log says why");

	s.port    = PortState::Taken;
	s.takenBy = Ip(192, 168, 1, 30);
	a         = PortAdvice(home, s, 2001);
	Check(a.tone == Tone::Warn && Has(a.text, "already sends UDP port 2001 to 192.168.1.30") &&
	          Has(a.text, "Point that at 192.168.1.24"),
	      "taken by another machine: which, and which it should be");

	s.lan  = Ip(192, 168, 1, 99);
	s.port = PortState::Failed;
	Check(Has(PortAdvice(home, s, 2001).text, "to 192.168.1.99"),
	      "the address the router was asked about wins over a guess");

	const std::vector<LocalAddress> hotspot = {{Ip(100, 72, 5, 6), true}};
	a = PortAdvice(hotspot, ReachStatus{}, 2001);
	Check(a.tone == Tone::Warn && Has(a.text, "carrier's shared address") &&
	          BehindCarrier(hotspot) && !RouterCanHelp(hotspot),
	      "behind a carrier: no router setting helps, and the router is not asked");
	Check(!BehindCarrier(home) && RouterCanHelp(home), "at home it is");
	Check(!RouterCanHelp({{Ip(203, 0, 113, 50), true}, {Ip(192, 168, 1, 24), true}}),
	      "and not when the machine has a public address of its own too");
}

void TestTheLogLines() {
	std::printf("\nwhat the log says\n");
	const std::vector<LocalAddress> home = {{Ip(192, 168, 1, 24), true}};
	ReachStatus                     s;
	s.lookup     = LookupState::Found;
	s.publicAddr = Ip(203, 0, 113, 9);
	Check(LookupLogLine(s, 2001) ==
	          "public address 203.0.113.9:2001: friends on the internet connect to that one",
	      "the address found");
	s.lookup    = LookupState::Failed;
	s.lookupWhy = "no answer from api.ipify.org, checkip.amazonaws.com";
	Check(Has(LookupLogLine(s, 2001), "could not look up the public address (no answer from") &&
	          Has(LookupLogLine(s, 2001), "a what-is-my-IP page shows it"),
	      "the lookup failing, with why, and the old advice");
	Check(Has(HostLogLine(home, 2001), "127.0.0.1:2001") &&
	          Has(HostLogLine(home, 2001), "192.168.1.24:2001"),
	      "how the host connects");
	Check(AsLogLine("UDP is fine.") == "UDP is fine" &&
	          AsLogLine("Windows Firewall is off.") == "Windows Firewall is off" &&
	          AsLogLine("Asking...") == "asking...",
	      "an abbreviation and a name keep their capitals, and an ellipsis its dots");

	const std::vector<std::string> lines = ReachLines(home, 2001, true);
	bool                           now   = false;
	for (const std::string &l : lines)
		now = now || Has(l, "being looked up now");
	Check(now, "the first advice says the public address is on its way");
}

FirewallFacts Firewall() {
	FirewallFacts f;
	f.known    = true;
	f.on       = true;
	f.profiles = FW_PROFILE_PRIVATE;
	return f;
}

FirewallRule For(const char *app, bool allow) {
	FirewallRule r;
	r.name            = allow ? "server.exe allow" : "server.exe";
	r.app             = app;
	r.allow           = allow;
	r.protocol        = FW_PROTOCOL_UDP;
	r.localPorts      = "*";
	r.remoteAddresses = "*";
	return r;
}

void TestWhatTheFirewallAllows() {
	std::printf("\nwhat Windows Firewall lets in\n");
	const std::string exe = "C:\\Games\\CoopIII\\server.exe";

	Check(JudgeFirewall(FirewallFacts{}, exe, 2001).state == FirewallState::Unknown,
	      "a firewall that could not be asked is unknown, not fine");
	FirewallFacts f = Firewall();
	f.on            = false;
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::Off, "off is off");

	f = Firewall();
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::NotAllowed,
	      "no rule, and what no rule mentions is dropped: not allowed");
	f.defaultAllows = true;
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::Allowed,
	      "unless the firewall lets in whatever no rule mentions");

	f = Firewall();
	f.rules.push_back(For("c:/games/COOPIII/Server.EXE", true));
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::Allowed,
	      "a rule for this program, spelt with other case and slashes, lets it in");

	f.rules.push_back(For("C:\\Games\\CoopIII\\server.exe", false));
	FirewallVerdict v = JudgeFirewall(f, exe, 2001);
	Check(v.state == FirewallState::Blocked && v.blockingRule == "server.exe",
	      "and a block beside it wins, by name, the way Windows' own does");

	f = Firewall();
	f.rules.push_back(For("C:\\Other\\server.exe", true));
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::NotAllowed,
	      "another copy's rule is not this one's");

	f                        = Firewall();
	FirewallRule tcp         = For(exe.c_str(), true);
	tcp.protocol             = 6;
	FirewallRule publicOnly  = For(exe.c_str(), true);
	publicOnly.profiles      = FW_PROFILE_PUBLIC;
	FirewallRule off         = For(exe.c_str(), true);
	off.enabled              = false;
	FirewallRule outbound    = For(exe.c_str(), true);
	outbound.inbound         = false;
	FirewallRule subnet      = For(exe.c_str(), true);
	subnet.remoteAddresses   = "LocalSubnet";
	f.rules                  = {tcp, publicOnly, off, outbound, subnet};
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::NotAllowed,
	      "TCP only, another kind of network, disabled, outbound, or the local subnet only: "
	      "none of them lets a friend in");

	f                   = Firewall();
	FirewallRule byPort = For("", true);
	byPort.localPorts   = "2000-2005,7777";
	f.rules             = {byPort};
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::Allowed,
	      "a rule for the port and any program does");
	Check(JudgeFirewall(f, exe, 2010).state == FirewallState::NotAllowed,
	      "on that port only");
	FirewallRule store = For("", true);
	store.package      = "S-1-15-2-1";
	store.localPorts   = "2001";
	FirewallRule svc   = For("", true);
	svc.service        = "dnscache";
	svc.localPorts     = "2001";
	f.rules            = {store, svc};
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::NotAllowed,
	      "a Store app's or a service's rule names no program and is still not ours");
	f.rules = {For("", true), For("", false)};
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::NotAllowed,
	      "and one for every program on every port, which is what a Store app's looks like "
	      "when its package goes unsaid, counts neither way");

	f                  = Firewall();
	f.blocksEverything = true;
	f.rules            = {For(exe.c_str(), true)};
	Check(JudgeFirewall(f, exe, 2001).state == FirewallState::BlocksEverything,
	      "\"block all incoming connections\" beats every rule");

	Check(PortListHas("*", 1) && PortListHas("", 1) && PortListHas("80,2001", 2001) &&
	          PortListHas("2000-2002", 2002) && !PortListHas("2000-2002", 2003) &&
	          !PortListHas("RPC", 2001) && !PortListHas("2001x", 2001),
	      "port lists: any, lists, ranges, and a keyword is no port");
}

void TestWhatTheHostIsTold() {
	std::printf("\nwhat the host is told about the firewall\n");
	const std::string exe = "C:\\Program Files\\CoopIII\\server.exe";

	FirewallVerdict v;
	v.state          = FirewallState::NotAllowed;
	FirewallAdvice a = AdviseFirewall(v, 2001);
	Check(a.tone == Tone::Warn && a.canFix && Has(a.detail, "UDP 2001"),
	      "not allowed: a warning, the port, and the button");
	v.state        = FirewallState::Blocked;
	v.blockingRule = "server.exe";
	a              = AdviseFirewall(v, 2001);
	Check(a.tone == Tone::Warn && a.canFix && Has(a.detail, "\"server.exe\""),
	      "blocked: which rule");
	v.state = FirewallState::BlocksEverything;
	a       = AdviseFirewall(v, 2001);
	Check(a.tone == Tone::Warn && !a.canFix, "block-everything: no button, since no rule helps");
	v.state = FirewallState::Allowed;
	Check(AdviseFirewall(v, 2001).tone == Tone::Ok, "allowed is good news");
	v.state = FirewallState::Unknown;
	Check(AdviseFirewall(v, 2001).tone == Tone::Info, "and unknown is neither");

	ShellCommand c = FirewallFix(exe, false);
	Check(c.file == "netsh.exe" &&
	          c.params == "advfirewall firewall add rule name=\"CoopIII server\" dir=in "
	                      "action=allow program=\"C:\\Program Files\\CoopIII\\server.exe\" "
	                      "protocol=UDP profile=any enable=yes",
	      "the button adds one inbound UDP rule for this program, the path quoted");
	c = FirewallFix(exe, true);
	Check(c.file == "cmd.exe" &&
	          c.params.rfind("/c \"netsh advfirewall firewall delete rule ", 0) == 0 &&
	          Has(c.params, "dir=in program=\"C:\\Program Files\\CoopIII\\server.exe\" & netsh "
	                        "advfirewall firewall add rule") &&
	          c.params.back() == '"',
	      "and when a rule blocks it, deletes the program's inbound rules first, in one go");

	v.state = FirewallState::NotAllowed;
	std::vector<std::string> lines = FirewallLogLines(v, 2001, exe);
	Check(lines.size() == 3 && lines[0].rfind("Windows Firewall has not allowed", 0) == 0 &&
	          Has(lines[2], "netsh advfirewall firewall add rule"),
	      "the console carries the command, for a machine with no window");
	v.state = FirewallState::Blocked;
	lines   = FirewallLogLines(v, 2001, exe);
	Check(lines.size() == 4 && Has(lines[2], "delete rule"), "and the delete, when it is blocked");
}

} // namespace

int RunPublicAddressTests() {
	g_ipFailures = 0;
	TestTheReplyIsAnAddressOrNothing();
	TestWhichAddressIsShown();
	TestWhatTheRouterLineSays();
	TestTheLogLines();
	TestWhatTheFirewallAllows();
	TestWhatTheHostIsTold();
	return g_ipFailures;
}
