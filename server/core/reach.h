// What somebody starting a server has to be told about how players reach it:
// which of this machine's addresses to hand out, and what the router needs
// when players are not on the same network. Pure, so the wording can be
// tested; the front ends list the addresses (server/addresses.cpp) and log
// these lines.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coopiii {

// IPv4 in host order, a.b.c.d as a << 24 | b << 16 | c << 8 | d.
enum class AddressKind : uint8_t {
	Loopback,    // 127/8
	LinkLocal,   // 169.254/16, what Windows gives itself with no DHCP
	Private,     // 10/8, 172.16/12, 192.168/16
	// 100.64/10, a carrier's NAT or a virtual network, and the two /8s
	// (25 and 26) that the VPN tools people use for games hand out. Public on
	// paper and never somebody's internet address at home.
	Virtual,
	Public,
};

// One of this machine's addresses, and whether its adapter has a default
// gateway behind it - the network players are actually on, rather than a
// virtual machine's or a tunnel's.
struct LocalAddress {
	uint32_t addr    = 0;
	bool     gateway = false;
};

inline AddressKind KindOf(uint32_t a) {
	const uint32_t b0 = a >> 24, b1 = (a >> 16) & 0xFF;
	if (b0 == 127)
		return AddressKind::Loopback;
	if (b0 == 169 && b1 == 254)
		return AddressKind::LinkLocal;
	if (b0 == 10 || (b0 == 172 && b1 >= 16 && b1 <= 31) || (b0 == 192 && b1 == 168))
		return AddressKind::Private;
	if ((b0 == 100 && b1 >= 64 && b1 <= 127) || b0 == 25 || b0 == 26)
		return AddressKind::Virtual;
	return AddressKind::Public;
}

inline std::string FormatAddress(uint32_t a) {
	char out[16];
	std::snprintf(out, sizeof out, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF,
	              a & 0xFF);
	return out;
}

// The address a player on the same network types in: a private one with a
// gateway behind it, or failing that any private one, or failing that 0.
inline uint32_t PreferredLanAddress(const std::vector<LocalAddress> &addresses) {
	for (const LocalAddress &a : addresses)
		if (KindOf(a.addr) == AddressKind::Private && a.gateway)
			return a.addr;
	for (const LocalAddress &a : addresses)
		if (KindOf(a.addr) == AddressKind::Private)
			return a.addr;
	return 0;
}

// `lookingUp` is the public address being asked for as this is said
// (ReachStatus below), so the forwarding line does not send somebody off to a
// what-is-my-IP page for an answer the server is about to give them.
inline std::vector<std::string> ReachLines(const std::vector<LocalAddress> &addresses,
                                           uint16_t port, bool lookingUp = false) {
	// On this network: the private addresses with a gateway, or all of them
	// when none has one. The rest are virtual machines' and tunnels' own, and
	// naming them sends a friend to the wrong one.
	bool anyGateway = false;
	for (const LocalAddress &a : addresses)
		anyGateway = anyGateway || (KindOf(a.addr) == AddressKind::Private && a.gateway);

	std::string lan, virt, carrier, open, self;
	for (const LocalAddress &a : addresses) {
		std::string *list = nullptr;
		switch (KindOf(a.addr)) {
		case AddressKind::Private:
			if (a.gateway || !anyGateway)
				list = &lan;
			break;
		case AddressKind::Virtual:
			// 100.64/10 with a gateway behind it is how this machine gets out:
			// a phone's hotspot or mobile internet, behind the carrier's own
			// NAT. Without one it is a VPN's, like 25/8 and 26/8.
			list = a.gateway && (a.addr >> 24) == 100 ? &carrier : &virt;
			break;
		case AddressKind::Public:    list = &open; break;
		case AddressKind::LinkLocal: list = &self; break;
		default: break;
		}
		if (!list)
			continue;
		const std::string one = FormatAddress(a.addr) + ":" + std::to_string(port);
		if (list->find(one) != std::string::npos)
			continue;
		if (!list->empty())
			*list += " or ";
		*list += one;
	}

	std::vector<std::string> lines;
	char                     line[512];
	if (!lan.empty()) {
		std::snprintf(line, sizeof line, "players on this network connect to %s", lan.c_str());
		lines.push_back(line);
	}
	if (!virt.empty()) {
		std::snprintf(line, sizeof line,
		              "players on the same VPN or virtual network as this machine connect to %s",
		              virt.c_str());
		lines.push_back(line);
	}
	if (!carrier.empty()) {
		std::snprintf(line, sizeof line,
		              "this machine is online through a carrier's shared address (%s), the kind "
		              "a phone's hotspot or mobile internet hands out: players on the internet "
		              "cannot connect through it and no router setting changes that. A VPN "
		              "both sides join works",
		              carrier.c_str());
		lines.push_back(line);
	}
	if (!open.empty()) {
		std::snprintf(line, sizeof line,
		              "this machine has a public address of its own: players on the internet "
		              "connect to %s",
		              open.c_str());
		lines.push_back(line);
	} else if (const uint32_t target = PreferredLanAddress(addresses)) {
		std::snprintf(line, sizeof line,
		              "players on the internet need UDP port %u forwarded on your router to "
		              "%s, then connect to the router's public address with :%u (%s)",
		              static_cast<unsigned>(port), FormatAddress(target).c_str(),
		              static_cast<unsigned>(port),
		              lookingUp ? "being looked up now" : "a what-is-my-IP page shows it");
		lines.push_back(line);
	}
	if (lan.empty() && virt.empty() && carrier.empty() && open.empty()) {
		if (!self.empty())
			std::snprintf(line, sizeof line,
			              "this machine only has an address it gave itself (%s), so no router "
			              "answered it: check its cable or Wi-Fi. Until then only a player "
			              "plugged straight into it can connect, to that address",
			              self.c_str());
		else
			std::snprintf(line, sizeof line,
			              "could not tell which address this machine has; players connect to "
			              "it with :%u",
			              static_cast<unsigned>(port));
		lines.push_back(line);
	}
	std::snprintf(line, sizeof line,
	              "if nobody can connect, the firewall has to let the server receive UDP on "
	              "port %u",
	              static_cast<unsigned>(port));
	lines.push_back(line);
	return lines;
}

// ---- the public address and the router ----------------------------------------
//
// Once a server starts, server/probe.cpp finds out in the background what the
// internet sees this network as (a what-is-my-IP service) and whether the
// router will forward the port by itself (UPnP). What follows is what the
// window and the log make of the answers, here with the rest so the wording
// and the choices can be tested.

// A what-is-my-IP reply: one dotted IPv4 address and nothing else but the
// whitespace around it. An IPv6 address, an error page or a captive portal's
// login is not an answer. A part with a leading zero is refused too, since
// some read 010 as octal.
inline bool ParseIpReply(const std::string &body, uint32_t *out) {
	const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	size_t b = 0, e = body.size();
	while (b < e && space(body[b]))
		++b;
	while (e > b && space(body[e - 1]))
		--e;
	if (e - b < 7 || e - b > 15)
		return false;

	uint32_t addr = 0;
	size_t   i    = b;
	for (int part = 0; part < 4; ++part) {
		if (part > 0) {
			if (i >= e || body[i] != '.')
				return false;
			++i;
		}
		const size_t start = i;
		uint32_t     v     = 0;
		while (i < e && i - start < 4 && body[i] >= '0' && body[i] <= '9')
			v = v * 10 + static_cast<uint32_t>(body[i++] - '0');
		const size_t digits = i - start;
		if (digits == 0 || digits > 3 || v > 255 || (digits > 1 && body[start] == '0'))
			return false;
		addr = addr << 8 | v;
	}
	if (i != e)
		return false;
	*out = addr;
	return true;
}

enum class LookupState : uint8_t {
	Off,       // the host turned it off
	Pending,
	Found,
	Failed,    // no service answered with an address
};

enum class PortState : uint8_t {
	Off,        // the host turned it off
	Pending,
	Opened,     // the router sends the port here now
	Taken,      // the router already sends it to another machine
	Failed,     // no router answered, or it said no
	NotNeeded,  // this machine is on the internet itself, with no router to ask
};

// How a line reads: plain, good news, or something the host has to act on.
enum class Tone : uint8_t { Info, Ok, Warn };

struct Advice {
	Tone        tone = Tone::Info;
	std::string text;
};

// What the background work has found so far.
struct ReachStatus {
	LookupState lookup     = LookupState::Off;
	uint32_t    publicAddr = 0;
	std::string lookupWhy;         // Failed: what went wrong, for the log
	PortState   port       = PortState::Off;
	uint32_t    lan        = 0;    // the address the router was asked to send the port to
	uint32_t    routerAddr = 0;    // the router's own internet address as it says it, 0 unknown
	uint32_t    takenBy    = 0;    // Taken: the machine the port already goes to
	std::string portWhy;           // Failed: what went wrong, for the log
};

// A public address on one of this machine's own adapters: it is on the
// internet itself, with no router in the way. One with a gateway first.
inline uint32_t DirectPublicAddress(const std::vector<LocalAddress> &addresses) {
	for (const LocalAddress &a : addresses)
		if (KindOf(a.addr) == AddressKind::Public && a.gateway)
			return a.addr;
	for (const LocalAddress &a : addresses)
		if (KindOf(a.addr) == AddressKind::Public)
			return a.addr;
	return 0;
}

// Online through a carrier's shared address and nothing else (ReachLines has
// the why): nobody on the internet gets in, whatever any router is told.
inline bool BehindCarrier(const std::vector<LocalAddress> &addresses) {
	if (PreferredLanAddress(addresses) || DirectPublicAddress(addresses))
		return false;
	for (const LocalAddress &a : addresses)
		if (KindOf(a.addr) == AddressKind::Virtual && (a.addr >> 24) == 100 && a.gateway)
			return true;
	return false;
}

// Whether asking the router is worth anything: there is a home network to
// send the port to, and this machine is not on the internet by itself.
inline bool RouterCanHelp(const std::vector<LocalAddress> &addresses) {
	return PreferredLanAddress(addresses) != 0 && !DirectPublicAddress(addresses);
}

// The address somebody on the same network types: PreferredLanAddress, or
// failing that the first one that is not self-assigned.
inline uint32_t ShareableLanAddress(const std::vector<LocalAddress> &addresses) {
	if (const uint32_t lan = PreferredLanAddress(addresses))
		return lan;
	for (const LocalAddress &a : addresses)
		if (KindOf(a.addr) != AddressKind::LinkLocal && KindOf(a.addr) != AddressKind::Loopback)
			return a.addr;
	return 0;
}

// The address friends on the internet type, or 0 when it is not known: the
// looked-up one, else this machine's own public one, else what the router
// said its own is when it opened the port.
inline uint32_t PublicAddressOf(const std::vector<LocalAddress> &addresses,
                                const ReachStatus &status) {
	if (status.lookup == LookupState::Found && KindOf(status.publicAddr) == AddressKind::Public)
		return status.publicAddr;
	if (const uint32_t direct = DirectPublicAddress(addresses))
		return direct;
	if (status.port == PortState::Opened && KindOf(status.routerAddr) == AddressKind::Public)
		return status.routerAddr;
	return 0;
}

inline std::string WithPort(uint32_t addr, uint16_t port) {
	return FormatAddress(addr) + ":" + std::to_string(port);
}

// The line about the router: whether the port is open, or what to do so it is.
inline Advice PortAdvice(const std::vector<LocalAddress> &addresses, const ReachStatus &status,
                         uint16_t port) {
	const unsigned p   = port;
	const uint32_t lan = status.lan ? status.lan : PreferredLanAddress(addresses);
	char           text[320];
	Advice         out;
	if (BehindCarrier(addresses)) {
		out.tone = Tone::Warn;
		out.text = "This connection is behind the carrier's shared address: nobody on the "
		           "internet can connect through it, and no router setting changes that. A VPN "
		           "both sides join works.";
		return out;
	}
	if (DirectPublicAddress(addresses) || status.port == PortState::NotNeeded) {
		out.tone = Tone::Ok;
		out.text = "This machine is on the internet itself: there is no router port to open.";
		return out;
	}
	switch (status.port) {
	case PortState::Pending:
		std::snprintf(text, sizeof text, "Asking your router to open UDP port %u...", p);
		break;
	case PortState::Opened:
		if (status.routerAddr && KindOf(status.routerAddr) != AddressKind::Public) {
			out.tone = Tone::Warn;
			std::snprintf(text, sizeof text,
			              "Port %u is open on your router, but the router is itself behind "
			              "another network (%s), so friends on the internet may still not get "
			              "through.",
			              p, FormatAddress(status.routerAddr).c_str());
		} else {
			out.tone = Tone::Ok;
			std::snprintf(text, sizeof text, "Port %u opened on your router automatically.", p);
		}
		break;
	case PortState::Taken:
		out.tone = Tone::Warn;
		std::snprintf(text, sizeof text,
		              "Your router already sends UDP port %u to %s. Point that at %s, or use "
		              "another port.",
		              p, FormatAddress(status.takenBy).c_str(),
		              lan ? FormatAddress(lan).c_str() : "this machine");
		break;
	case PortState::Failed:
		out.tone = Tone::Warn;
		std::snprintf(text, sizeof text,
		              "Could not open the port automatically. Forward UDP %u to %s on your "
		              "router.",
		              p, lan ? FormatAddress(lan).c_str() : "this machine");
		break;
	default:   // Off
		if (lan)
			std::snprintf(text, sizeof text,
			              "For friends outside your network, forward UDP port %u on your router "
			              "to %s.",
			              p, FormatAddress(lan).c_str());
		else
			std::snprintf(text, sizeof text,
			              "For friends outside your network, forward UDP port %u on your "
			              "router.",
			              p);
		break;
	}
	out.text = text;
	return out;
}

// What the window's header shows: the address to hand out, the same
// network's under its own name, and the two lines under them.
struct ShareView {
	const char *label    = "Address to share";
	std::string address;          // a.b.c.d:port, or empty while it is being looked up
	bool        isPublic = false;
	std::string lan;              // the same network's, when the main one is not it
	Advice      port;             // PortAdvice
	Advice      note;             // the lookup failing, or how the host connects; may be empty
};

inline ShareView ShareFor(const std::vector<LocalAddress> &addresses, const ReachStatus &status,
                          uint16_t port) {
	ShareView      view;
	const unsigned p          = port;
	const uint32_t pub        = PublicAddressOf(addresses, status);
	const uint32_t lan        = ShareableLanAddress(addresses);
	const bool     showPublic = pub != 0 || status.lookup == LookupState::Pending;
	char           text[320];

	if (showPublic) {
		view.label    = "Public address";
		view.isPublic = pub != 0;
		if (pub)
			view.address = WithPort(pub, port);
		if (lan && lan != pub)
			view.lan = WithPort(lan, port);
	} else {
		view.address = lan ? WithPort(lan, port) : "127.0.0.1:" + std::to_string(port);
	}
	view.port = PortAdvice(addresses, status, port);

	if (status.lookup == LookupState::Failed && !pub) {
		std::snprintf(text, sizeof text,
		              "Could not look up your public address. Friends connect to your router's "
		              "public address with :%u; a what-is-my-IP page shows it.",
		              p);
		view.note.text = text;
	} else if (pub && pub == DirectPublicAddress(addresses)) {
		std::snprintf(text, sizeof text, "On this PC, connect with 127.0.0.1:%u.", p);
		view.note.text = text;
	} else if (showPublic) {
		// Hairpinning: plenty of routers drop a packet from inside addressed to
		// their own outside, so the host typing the public address sees the
		// server as down while everybody else is playing on it.
		std::snprintf(text, sizeof text,
		              "On this PC, connect with 127.0.0.1:%u. Many routers do not loop the "
		              "public address back to your own network.",
		              p);
		view.note.text = text;
	}
	return view;
}

// An Advice's text the way the log says things: from a lower-case letter and
// without the full stop. UDP and Windows keep their capitals.
inline std::string AsLogLine(std::string text) {
	if (text.size() > 1 && text[0] >= 'A' && text[0] <= 'Z' && text[1] >= 'a' && text[1] <= 'z' &&
	    text.compare(0, 8, "Windows ") != 0)
		text[0] = static_cast<char>(text[0] - 'A' + 'a');
	if (!text.empty() && text.back() == '.' &&
	    (text.size() < 3 || text.compare(text.size() - 3, 3, "...") != 0))
		text.pop_back();
	return text;
}

// What the log says once the lookup has an answer, or has given up.
inline std::string LookupLogLine(const ReachStatus &status, uint16_t port) {
	char line[320];
	if (status.lookup == LookupState::Found)
		std::snprintf(line, sizeof line,
		              "public address %s: friends on the internet connect to that one",
		              WithPort(status.publicAddr, port).c_str());
	else
		std::snprintf(line, sizeof line,
		              "could not look up the public address (%s). Friends on the internet "
		              "connect to your router's public address with :%u; a what-is-my-IP page "
		              "shows it",
		              status.lookupWhy.empty() ? "no answer" : status.lookupWhy.c_str(),
		              static_cast<unsigned>(port));
	return line;
}

// And once the router has answered.
inline std::string PortLogLine(const std::vector<LocalAddress> &addresses,
                               const ReachStatus &status, uint16_t port) {
	std::string line = AsLogLine(PortAdvice(addresses, status, port).text);
	if (status.port == PortState::Failed && !status.portWhy.empty())
		line += " (" + status.portWhy + ")";
	return line;
}

// The host's own game: where it connects. Said once the public address is
// known, since that is when somebody tries it from the same machine.
inline std::string HostLogLine(const std::vector<LocalAddress> &addresses, uint16_t port) {
	const uint32_t lan = ShareableLanAddress(addresses);
	char           line[320];
	std::snprintf(line, sizeof line,
	              "on this PC, connect with 127.0.0.1:%u%s%s: many routers do not loop the "
	              "public address back to your own network",
	              static_cast<unsigned>(port), lan ? ", and on this network with " : "",
	              lan ? WithPort(lan, port).c_str() : "");
	return line;
}

} // namespace coopiii
