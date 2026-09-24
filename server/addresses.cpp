// This machine's IPv4 addresses, for telling whoever starts the server what to
// hand out (server/core/reach.h). Interfaces that are up; loopback and tunnels
// left out, and each with whether a default gateway sits behind its adapter.
#include "run.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace coopiii {

std::vector<LocalAddress> LocalIPv4Addresses() {
	const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
	                    GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG                size = 16 * 1024;
	std::vector<uint8_t> buffer(size);
	auto                *table  = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
	ULONG                result = GetAdaptersAddresses(AF_INET, flags, nullptr, table, &size);
	if (result == ERROR_BUFFER_OVERFLOW) {
		buffer.resize(size);
		table  = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
		result = GetAdaptersAddresses(AF_INET, flags, nullptr, table, &size);
	}

	std::vector<LocalAddress> out;
	if (result != NO_ERROR)
		return out;
	for (const IP_ADAPTER_ADDRESSES *a = table; a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
		    a->IfType == IF_TYPE_TUNNEL)
			continue;
		const bool gateway = a->FirstGatewayAddress != nullptr;
		for (const IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
			if (u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			const auto    *in   = reinterpret_cast<const sockaddr_in *>(u->Address.lpSockaddr);
			const uint32_t addr = ntohl(in->sin_addr.s_addr);
			if ((addr >> 24) == 127)
				continue;
			const auto same = std::find_if(out.begin(), out.end(),
			                               [addr](const LocalAddress &l) { return l.addr == addr; });
			if (same == out.end())
				out.push_back(LocalAddress{addr, gateway});
			else
				same->gateway = same->gateway || gateway;
		}
	}
	return out;
}

} // namespace coopiii
