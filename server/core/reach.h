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

inline std::vector<std::string> ReachLines(const std::vector<LocalAddress> &addresses,
                                           uint16_t port) {
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
		              "%s, then connect to the router's public address with :%u (a "
		              "what-is-my-IP page shows it)",
		              static_cast<unsigned>(port), FormatAddress(target).c_str(),
		              static_cast<unsigned>(port));
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

} // namespace coopiii
