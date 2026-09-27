// Finding out how the internet reaches this server, without holding it up.
//
// When a server starts, a thread of its own asks Windows Firewall whether
// server.exe may receive UDP, asks a what-is-my-IP service for this network's
// public address, and asks the router over UPnP to forward the port here.
// Each can take seconds or never answer, and the session has to keep ticking
// and the window drawing meanwhile, so the front ends only ever read what it
// has found so far (Status, Firewall) and pass on what it has to say
// (TakeLines). server/core/reach.h and firewall.h are what the answers mean.
//
// The port it opened is closed again by End, or by Finish on the way out.
// A crash leaves it open; the next start finds it pointing here under our
// name, takes it over, and closes it on the next clean stop.
#pragma once

#include "firewall.h"
#include "reach.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coopiii {

struct ProbeLine {
	bool        warn = false;
	std::string text;
};

class ReachProbe {
public:
	ReachProbe();
	~ReachProbe();

	ReachProbe(const ReachProbe &)            = delete;
	ReachProbe &operator=(const ReachProbe &) = delete;

	// A server has started on `port`: find out, in the background, whatever
	// the two settings allow. `local` is this machine's addresses
	// (LocalIPv4Addresses), taken once rather than on every question.
	void Begin(uint16_t port, const std::vector<LocalAddress> &local, bool lookUp, bool openPort);

	// It has stopped: whatever Begin was still doing is dropped, and the
	// router is told to close the port if it was opened. Returns at once.
	void End();

	ReachStatus     Status() const;
	FirewallVerdict Firewall() const;

	// What it has found out since the last call, for the log.
	std::vector<ProbeLine> TakeLines();

	// The window's Allow button: runs FirewallFix elevated, so Windows asks
	// the host first, then looks at the firewall again. `hwnd` is the window
	// the permission prompt belongs to.
	void AllowInFirewall(void *hwnd);
	bool Allowing() const;

	// On the way out: End, then wait up to `timeoutMs` for the router to be
	// told. A router that never answers does not keep the process alive.
	void Finish(uint32_t timeoutMs = 8000);

	// This executable, as the firewall names it.
	static std::string ExePath();

	struct Shared;

private:
	std::shared_ptr<Shared> m_shared;
};

} // namespace coopiii
