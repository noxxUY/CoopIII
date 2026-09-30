// Whether Windows Firewall lets the server receive its UDP, worked out from
// the rules, and what to tell the host when it does not.
//
// server/probe.cpp reads the rules through INetFwPolicy2 and hands them over
// as FirewallFacts; everything here is pure, so what counts as allowed and
// what the window says about it can be tested without a firewall. Nothing
// here changes a rule. FirewallFix is the command the window runs, elevated,
// only when the host presses its button.
#pragma once

#include "reach.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coopiii {

// INetFwRule's protocol numbers and profile bits (netfw.h), spelled out so
// this header does not need Windows.
constexpr int32_t FW_PROTOCOL_UDP    = 17;
constexpr int32_t FW_PROTOCOL_ANY    = 256;
constexpr int32_t FW_PROFILE_DOMAIN  = 1;
constexpr int32_t FW_PROFILE_PRIVATE = 2;
constexpr int32_t FW_PROFILE_PUBLIC  = 4;
constexpr int32_t FW_PROFILE_ALL     = 0x7FFFFFFF;

// One inbound or outbound rule, the parts of it that decide whether it is
// about the server. Strings as the firewall gives them, environment
// variables in a program's path already expanded.
struct FirewallRule {
	std::string name;
	std::string app;              // empty: every program
	std::string service;          // not empty: only that service
	std::string package;          // not empty: only that Store app, or that user's
	std::string localPorts;       // "*", "2001", "2000-2010,3000"
	std::string remoteAddresses;  // "*", or only some
	int32_t     protocol = FW_PROTOCOL_ANY;
	int32_t     profiles = FW_PROFILE_ALL;
	bool        enabled  = true;
	bool        inbound  = true;
	bool        allow    = true;
};

// The firewall as it stands for the networks this machine is on now.
struct FirewallFacts {
	bool    known            = false;   // it could be asked at all
	bool    on               = false;   // on for at least one of them
	bool    blocksEverything = false;   // "Block all incoming connections", which no rule passes
	bool    defaultAllows    = false;   // lets in what no rule mentions; not Windows' default
	int32_t profiles         = 0;       // FW_PROFILE_* of the networks this machine is on
	std::vector<FirewallRule> rules;
};

enum class FirewallState : uint8_t {
	Unknown,            // could not be asked
	Off,
	Allowed,
	NotAllowed,         // no rule lets it in, and what no rule mentions is dropped
	Blocked,            // a rule stops it, and a block beats any allow
	BlocksEverything,
};

struct FirewallVerdict {
	FirewallState state = FirewallState::Unknown;
	std::string   blockingRule;   // Blocked: the rule's name
};

// Whether a rule's port list takes `port`: "*" or nothing for any, otherwise
// numbers and ranges split by commas. The keywords some system rules use
// ("RPC", "IPHTTPSIn") are never the server's port.
inline bool PortListHas(const std::string &list, uint16_t port) {
	if (list.empty() || list == "*")
		return true;
	size_t at = 0;
	while (at <= list.size()) {
		size_t end = list.find(',', at);
		if (end == std::string::npos)
			end = list.size();
		const std::string item = list.substr(at, end - at);
		at                     = end + 1;

		const char *s    = item.c_str();
		char       *stop = nullptr;
		const long  lo   = std::strtol(s, &stop, 10);
		if (stop == s)
			continue;
		long hi = lo;
		if (*stop == '-') {
			const char *from = stop + 1;
			hi               = std::strtol(from, &stop, 10);
			if (stop == from)
				continue;
		}
		if (*stop != '\0')
			continue;
		if (port >= lo && port <= hi)
			return true;
	}
	return false;
}

// Two Windows paths naming the same file, as far as text can tell: case and
// the direction of the slashes do not matter.
inline bool SamePath(const std::string &a, const std::string &b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		char x = a[i], y = b[i];
		if (x == '/')
			x = '\\';
		if (y == '/')
			y = '\\';
		if (x >= 'A' && x <= 'Z')
			x = static_cast<char>(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = static_cast<char>(y - 'A' + 'a');
		if (x != y)
			return false;
	}
	return true;
}

// Whether a rule is about UDP arriving at `exe` on `port` from anywhere, on
// the networks the machine is on now.
inline bool RuleCovers(const FirewallRule &r, const FirewallFacts &f, const std::string &exe,
                       uint16_t port) {
	if (!r.enabled || !r.inbound || (r.profiles & f.profiles) == 0)
		return false;
	if (r.protocol != FW_PROTOCOL_UDP && r.protocol != FW_PROTOCOL_ANY)
		return false;
	if (!r.service.empty() || !r.package.empty())
		return false;
	if (!r.app.empty() && !SamePath(r.app, exe))
		return false;
	// A rule for every program on every port is, on a real machine, almost
	// always a Store app's that the COM interface does not name the package
	// of: Windows 11 has dozens. A rule for no program in particular counts
	// only when it names the port.
	if (r.app.empty() && (r.localPorts.empty() || r.localPorts == "*"))
		return false;
	// A rule for some remote addresses only, "LocalSubnet" say, is not one
	// that lets a friend on the internet in, nor one that keeps everybody out.
	if (!r.remoteAddresses.empty() && r.remoteAddresses != "*")
		return false;
	return PortListHas(r.localPorts, port);
}

inline FirewallVerdict JudgeFirewall(const FirewallFacts &f, const std::string &exe,
                                     uint16_t port) {
	FirewallVerdict v;
	if (!f.known)
		return v;
	if (!f.on) {
		v.state = FirewallState::Off;
		return v;
	}
	if (f.blocksEverything) {
		v.state = FirewallState::BlocksEverything;
		return v;
	}
	bool allowed = false;
	for (const FirewallRule &r : f.rules) {
		if (!RuleCovers(r, f, exe, port))
			continue;
		if (!r.allow) {
			v.state        = FirewallState::Blocked;
			v.blockingRule = r.name;
			return v;
		}
		allowed = true;
	}
	v.state = allowed || f.defaultAllows ? FirewallState::Allowed : FirewallState::NotAllowed;
	return v;
}

// What the window shows. Only a Warn is worth a banner; the rest go to the
// log.
struct FirewallAdvice {
	Tone        tone = Tone::Info;
	std::string title;
	std::string detail;
	bool        canFix = false;   // the Allow button would help
};

inline FirewallAdvice AdviseFirewall(const FirewallVerdict &v, uint16_t port) {
	FirewallAdvice a;
	const std::string udp = "UDP " + std::to_string(port);
	switch (v.state) {
	case FirewallState::Off:
		a.tone  = Tone::Ok;
		a.title = "Windows Firewall is off on this network";
		break;
	case FirewallState::Allowed:
		a.tone  = Tone::Ok;
		a.title = "Windows Firewall lets the server in";
		break;
	case FirewallState::NotAllowed:
		a.tone   = Tone::Warn;
		a.title  = "Windows Firewall has not allowed the server yet";
		a.detail = "Nobody on another machine can connect until it lets server.exe receive " +
		           udp + ". If Windows asked, choose Allow.";
		a.canFix = true;
		break;
	case FirewallState::Blocked:
		a.tone   = Tone::Warn;
		a.title  = "Windows Firewall is blocking the server";
		a.detail = "A rule named \"" + v.blockingRule + "\" stops server.exe receiving " + udp +
		           ", and nobody on another machine can connect while it is there.";
		a.canFix = true;
		break;
	case FirewallState::BlocksEverything:
		a.tone   = Tone::Warn;
		a.title  = "Windows Firewall blocks every incoming connection";
		a.detail = "\"Block all incoming connections\" is ticked for this network and no rule "
		           "gets past it. Untick it in Windows Security, under Firewall & network "
		           "protection.";
		break;
	default:
		a.title  = "Could not read Windows Firewall's rules";
		a.detail = "If nobody can connect, it has to let server.exe receive " + udp + ".";
		break;
	}
	return a;
}

// The command behind the Allow button, run elevated so Windows asks first: an
// inbound rule for this server.exe and UDP, on any port so that changing the
// server's does not need another. A block beats any allow, so when a rule
// blocks it (the one Windows writes when its first prompt is cancelled), the
// program's other inbound rules go first; that is deleting rules, which is
// why it happens only on the button, and why the button's tooltip says so.
struct ShellCommand {
	std::string file;
	std::string params;
};

constexpr const char *FIREWALL_RULE_NAME = "CoopIII server";

inline std::string FirewallAddRule(const std::string &exe) {
	return std::string("advfirewall firewall add rule name=\"") + FIREWALL_RULE_NAME +
	       "\" dir=in action=allow program=\"" + exe + "\" protocol=UDP profile=any enable=yes";
}

inline std::string FirewallDeleteRules(const std::string &exe) {
	return "advfirewall firewall delete rule name=all dir=in program=\"" + exe + "\"";
}

inline ShellCommand FirewallFix(const std::string &exe, bool removeBlocking) {
	if (!removeBlocking)
		return {"netsh.exe", FirewallAddRule(exe)};
	// cmd /c with the whole line in one more pair of quotes: cmd takes off
	// the first and the last and runs what is between exactly as written, the
	// program's own quotes included.
	return {"cmd.exe", "/c \"netsh " + FirewallDeleteRules(exe) + " & netsh " +
	                       FirewallAddRule(exe) + "\""};
}

// The log's version, which on a machine with no window has to carry the
// command itself.
inline std::vector<std::string> FirewallLogLines(const FirewallVerdict &v, uint16_t port,
                                                 const std::string &exe) {
	const FirewallAdvice     a = AdviseFirewall(v, port);
	std::vector<std::string> lines;
	std::string              first = AsLogLine(a.title);
	if (!a.detail.empty())
		first += ": " + AsLogLine(a.detail);
	lines.push_back(first);
	if (a.canFix) {
		lines.push_back("to allow it, run this in a command prompt opened as administrator:");
		if (v.state == FirewallState::Blocked)
			lines.push_back("  netsh " + FirewallDeleteRules(exe));
		lines.push_back("  netsh " + FirewallAddRule(exe));
	}
	return lines;
}

} // namespace coopiii
