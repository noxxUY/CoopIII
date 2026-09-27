// probe.h says what this is. Three conversations with Windows, all of them on
// the probe's own thread: WinHTTP for the public address, the NATUPnP COM
// objects (hnetcfg.dll) for the router, and INetFwPolicy2 for the firewall.
// The only thing that changes anything is the Allow button's netsh, and that
// goes through Windows' own permission prompt.
#include "probe.h"

#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>
#include <natupnp.h>
#include <netfw.h>
#include <shellapi.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

namespace coopiii {
namespace {

std::string Narrow(const wchar_t *text, int length = -1) {
	if (!text)
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return {};
	std::string out(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, length, &out[0], n, nullptr, nullptr);
	if (length < 0 && !out.empty() && out.back() == '\0')
		out.pop_back();
	return out;
}

std::wstring Widen(const std::string &text) {
	if (text.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
	                                  nullptr, 0);
	std::wstring out(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], n);
	return out;
}

// A BSTR that frees itself.
class Bstr {
public:
	Bstr() = default;
	explicit Bstr(const wchar_t *text) : m_s(SysAllocString(text)) {}
	~Bstr() { SysFreeString(m_s); }
	Bstr(const Bstr &)            = delete;
	Bstr &operator=(const Bstr &) = delete;

	BSTR *Out() {
		SysFreeString(m_s);
		m_s = nullptr;
		return &m_s;
	}
	operator BSTR() const { return m_s; }
	std::string Str() const { return m_s ? Narrow(m_s, static_cast<int>(SysStringLen(m_s))) : ""; }

private:
	BSTR m_s = nullptr;
};

// A COM pointer that releases itself.
template <class T>
struct Com {
	T *p = nullptr;
	Com() = default;
	~Com() { Reset(); }
	Com(const Com &)            = delete;
	Com &operator=(const Com &) = delete;

	void Reset() {
		if (p)
			p->Release();
		p = nullptr;
	}
	T **Out() {
		Reset();
		return &p;
	}
	void **Void() { return reinterpret_cast<void **>(Out()); }
	T   *operator->() const { return p; }
	explicit operator bool() const { return p != nullptr; }
};

std::string WithCode(const char *what, HRESULT hr) {
	char out[160];
	std::snprintf(out, sizeof out, "%s, 0x%08lX", what, static_cast<unsigned long>(hr));
	return out;
}

uint32_t AddressIn(const std::string &text) {
	uint32_t addr = 0;
	return ParseIpReply(text, &addr) ? addr : 0;
}

// ---- the public address ----------------------------------------------------------

// Three services that answer a GET of their front page with the caller's
// address as plain text and nothing else, tried in order until one does.
// Each only ever gives IPv4, which is all ENet listens on: a service that
// can answer in IPv6 would hand a machine with both an address nobody can
// connect to.
struct Service {
	const wchar_t *host;
	const char    *name;
};
const Service kServices[] = {
    {L"api.ipify.org", "api.ipify.org"},
    {L"checkip.amazonaws.com", "checkip.amazonaws.com"},
    {L"ipv4.icanhazip.com", "ipv4.icanhazip.com"},
};

bool Fetch(HINTERNET session, const wchar_t *host, std::string *body) {
	bool      ok         = false;
	HINTERNET connection = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!connection)
		return false;
	HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
	                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (request) {
		if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
		                       0, 0, 0) &&
		    WinHttpReceiveResponse(request, nullptr)) {
			DWORD status = 0, size = sizeof(status);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
			                    WINHTTP_NO_HEADER_INDEX);
			if (status == 200) {
				// An address is fifteen characters. Anything much longer is
				// not one, and there is no reason to read all of it.
				char  buf[128];
				DWORD read = 0;
				body->clear();
				while (body->size() < 256 && WinHttpReadData(request, buf, sizeof buf, &read) &&
				       read > 0)
					body->append(buf, read);
				ok = true;
			}
		}
		WinHttpCloseHandle(request);
	}
	WinHttpCloseHandle(connection);
	return ok;
}

uint32_t LookUpPublicAddress(const std::function<bool()> &stale, std::string *why) {
	HINTERNET session = WinHttpOpen(L"CoopIII Server", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		*why = "could not start a connection";
		return 0;
	}
	// Short. The answer is a dozen bytes, and a service that has not given it
	// in a few seconds is not going to.
	WinHttpSetTimeouts(session, 3000, 3000, 3000, 4000);

	uint32_t    found = 0;
	std::string asked;
	for (const Service &s : kServices) {
		if (stale())
			break;
		std::string body;
		uint32_t    addr = 0;
		if (Fetch(session, s.host, &body) && ParseIpReply(body, &addr) &&
		    KindOf(addr) == AddressKind::Public) {
			found = addr;
			break;
		}
		asked += asked.empty() ? "" : ", ";
		asked += s.name;
	}
	WinHttpCloseHandle(session);
	if (!found)
		*why = "no answer from " + asked;
	return found;
}

// ---- the router ------------------------------------------------------------------

constexpr const wchar_t *kMappingName = L"CoopIII server";

// What was opened, so it can be closed. `owned` is false for a forward that
// was already there pointing here under somebody else's name: that one is
// used and left alone.
struct Router {
	IStaticPortMappingCollection *mappings = nullptr;
	uint16_t                      port     = 0;
	bool                          owned    = false;

	void Forget() {
		if (mappings)
			mappings->Release();
		mappings = nullptr;
		port     = 0;
		owned    = false;
	}
};

void OpenPort(Router &router, uint16_t port, uint32_t lan, const std::function<bool()> &stale,
              ReachStatus *out) {
	out->port = PortState::Failed;

	Com<IUPnPNAT> nat;
	HRESULT       hr = CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_INPROC_SERVER,
	                                    __uuidof(IUPnPNAT), nat.Void());
	if (FAILED(hr) || !nat) {
		out->portWhy = WithCode("UPnP is not available on this machine", hr);
		return;
	}

	// The collection comes back empty-handed both while the search for the
	// router is still going on and when there is no UPnP router at all. A
	// few tries a second and a half apart tell the two apart.
	Com<IStaticPortMappingCollection> mappings;
	for (int attempt = 0; attempt < 4 && !mappings && !stale(); ++attempt) {
		if (attempt > 0)
			Sleep(1500);
		hr = nat->get_StaticPortMappingCollection(mappings.Out());
	}
	if (!mappings) {
		out->portWhy = "no router answered over UPnP; it may have UPnP turned off";
		return;
	}

	Bstr                    udp(L"UDP");
	Com<IStaticPortMapping> mapping;
	bool                    ours = true;
	if (SUCCEEDED(mappings->get_Item(port, udp, mapping.Out())) && mapping) {
		// Already forwarded. Here, and it is ours to use; anywhere else, and
		// it is not ours to take.
		Bstr client, name;
		long internal = 0;
		mapping->get_InternalClient(client.Out());
		mapping->get_InternalPort(&internal);
		mapping->get_Description(name.Out());
		const uint32_t to = AddressIn(client.Str());
		if (to != lan) {
			if (to) {
				out->port    = PortState::Taken;
				out->takenBy = to;
			} else {
				out->portWhy = "the router already forwards it to " + client.Str();
			}
			return;
		}
		if (internal != port) {
			char why[96];
			std::snprintf(why, sizeof why, "the router already forwards it to port %ld here",
			              internal);
			out->portWhy = why;
			return;
		}
		ours = name.Str() == Narrow(kMappingName);
		VARIANT_BOOL enabled = VARIANT_FALSE;
		mapping->get_Enabled(&enabled);
		if (enabled != VARIANT_TRUE)
			mapping->Enable(VARIANT_TRUE);
	} else {
		const std::wstring to = Widen(FormatAddress(lan));
		Bstr               client(to.c_str()), name(kMappingName);
		hr = mappings->Add(port, udp, port, client, VARIANT_TRUE, name, mapping.Out());
		if (FAILED(hr) || !mapping) {
			out->portWhy = WithCode("the router said no", hr);
			return;
		}
	}

	Bstr external;
	mapping->get_ExternalIPAddress(external.Out());
	out->routerAddr = AddressIn(external.Str());
	out->port       = PortState::Opened;

	router.Forget();
	router.mappings = mappings.p;
	mappings.p      = nullptr;
	router.port     = port;
	router.owned    = ours;
}

bool ClosePort(Router &router) {
	HRESULT hr = S_OK;
	if (router.mappings && router.owned) {
		Bstr udp(L"UDP");
		hr = router.mappings->Remove(router.port, udp);
	}
	router.Forget();
	return SUCCEEDED(hr);
}

// ---- the firewall ----------------------------------------------------------------

std::string Expanded(const std::string &path) {
	if (path.find('%') == std::string::npos)
		return path;
	const std::wstring wide = Widen(path);
	wchar_t            out[MAX_PATH * 2];
	const DWORD        n = ExpandEnvironmentStringsW(wide.c_str(), out, MAX_PATH * 2);
	return n > 0 && n <= MAX_PATH * 2 ? Narrow(out) : path;
}

FirewallFacts ReadFirewall() {
	FirewallFacts        facts;
	Com<INetFwPolicy2>   policy;
	if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
	                            __uuidof(INetFwPolicy2), policy.Void())) ||
	    !policy)
		return facts;

	long profiles = 0;
	if (FAILED(policy->get_CurrentProfileTypes(&profiles)))
		return facts;
	facts.profiles = profiles;

	bool on = false, blockAll = false, defaultAllows = true;
	for (const NET_FW_PROFILE_TYPE2 p :
	     {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
		if ((profiles & p) == 0)
			continue;
		VARIANT_BOOL enabled = VARIANT_FALSE;
		if (FAILED(policy->get_FirewallEnabled(p, &enabled)) || enabled != VARIANT_TRUE)
			continue;
		on                   = true;
		VARIANT_BOOL all     = VARIANT_FALSE;
		NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
		if (SUCCEEDED(policy->get_BlockAllInboundTraffic(p, &all)) && all == VARIANT_TRUE)
			blockAll = true;
		if (FAILED(policy->get_DefaultInboundAction(p, &action)) || action != NET_FW_ACTION_ALLOW)
			defaultAllows = false;
	}
	facts.on               = on;
	facts.blocksEverything = blockAll;
	facts.defaultAllows    = on && defaultAllows;

	Com<INetFwRules> rules;
	Com<IUnknown>    walk;
	Com<IEnumVARIANT> each;
	if (FAILED(policy->get_Rules(rules.Out())) || !rules ||
	    FAILED(rules->get__NewEnum(walk.Out())) || !walk ||
	    FAILED(walk->QueryInterface(__uuidof(IEnumVARIANT), each.Void())) || !each)
		return facts;

	VARIANT item;
	VariantInit(&item);
	ULONG fetched = 0;
	while (each->Next(1, &item, &fetched) == S_OK && fetched == 1) {
		Com<INetFwRule> rule;
		if (item.vt == VT_DISPATCH && item.pdispVal)
			item.pdispVal->QueryInterface(__uuidof(INetFwRule), rule.Void());
		VariantClear(&item);
		if (!rule)
			continue;

		NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
		rule->get_Direction(&direction);
		if (direction != NET_FW_RULE_DIR_IN)
			continue;

		FirewallRule r;
		VARIANT_BOOL enabled = VARIANT_FALSE;
		NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
		long          protocol = FW_PROTOCOL_ANY, ruleProfiles = FW_PROFILE_ALL;
		rule->get_Enabled(&enabled);
		rule->get_Action(&action);
		rule->get_Protocol(&protocol);
		rule->get_Profiles(&ruleProfiles);
		r.enabled  = enabled == VARIANT_TRUE;
		r.inbound  = true;
		r.allow    = action == NET_FW_ACTION_ALLOW;
		r.protocol = protocol;
		r.profiles = ruleProfiles;
		if (!r.enabled)
			continue;

		Bstr name, app, service, ports, remote;
		rule->get_Name(name.Out());
		rule->get_ApplicationName(app.Out());
		rule->get_ServiceName(service.Out());
		rule->get_LocalPorts(ports.Out());
		rule->get_RemoteAddresses(remote.Out());
		r.name            = name.Str();
		r.app             = Expanded(app.Str());
		r.service         = service.Str();
		r.localPorts      = ports.Str();
		r.remoteAddresses = remote.Str();

		// A Store app's rules name no program and no port, and would read as
		// "everything allowed" without what they are really about. Measured
		// on Windows 11: the package id comes back empty for them, and the
		// owning user's SID is what they carry instead, so either will do.
		Com<INetFwRule3> third;
		if (SUCCEEDED(rule->QueryInterface(__uuidof(INetFwRule3), third.Void())) && third) {
			Bstr package, owner;
			third->get_LocalAppPackageId(package.Out());
			third->get_LocalUserOwner(owner.Out());
			r.package = !package.Str().empty() ? package.Str() : owner.Str();
		}
		facts.rules.push_back(std::move(r));
	}
	facts.known = true;
	return facts;
}

bool InTheWay(FirewallState state) {
	return state == FirewallState::NotAllowed || state == FirewallState::Blocked ||
	       state == FirewallState::BlocksEverything;
}

} // namespace

// ---- the thread --------------------------------------------------------------------

struct ReachProbe::Shared {
	struct Job {
		enum Kind { Begin, End, Recheck, Quit } kind = Recheck;
		uint16_t                  port       = 0;
		std::vector<LocalAddress> local;
		bool                      lookUp     = false;
		bool                      openPort   = false;
		bool                      announce   = false;   // Recheck: say it even unchanged
		uint32_t                  generation = 0;
	};

	mutable std::mutex      mutex;
	std::condition_variable wake;
	std::condition_variable done;
	std::deque<Job>         jobs;
	ReachStatus             status;
	FirewallVerdict         firewall;
	std::vector<ProbeLine>  lines;
	uint32_t                generation = 0;   // one more for every Begin and End
	bool                    running    = false;
	bool                    quitting   = false;
	bool                    finished   = false;
	bool                    allowing   = false;
	std::thread             worker;

	void Queue(Job job) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			jobs.push_back(std::move(job));
		}
		wake.notify_one();
	}

	void Say(bool warn, std::string text) {
		std::lock_guard<std::mutex> lock(mutex);
		lines.push_back({warn, std::move(text)});
	}

	bool Stale(uint32_t gen) const {
		std::lock_guard<std::mutex> lock(mutex);
		return quitting || generation != gen;
	}

	// Writes into the status unless the server has stopped or started again
	// since `gen` was handed out, in which case the answer is about a server
	// that is not there any more. The copy it made is in `now`.
	template <class F>
	bool Publish(uint32_t gen, ReachStatus *now, F &&change) {
		std::lock_guard<std::mutex> lock(mutex);
		if (quitting || generation != gen)
			return false;
		change(status);
		*now = status;
		return true;
	}

	void CheckFirewall(uint16_t port, bool announce) {
		const std::string     exe = ExePath();
		const FirewallVerdict v   = JudgeFirewall(ReadFirewall(), exe, port);
		bool                  changed;
		{
			std::lock_guard<std::mutex> lock(mutex);
			changed  = v.state != firewall.state || v.blockingRule != firewall.blockingRule;
			firewall = v;
		}
		if (!announce && !changed)
			return;
		const bool warn = AdviseFirewall(v, port).tone == Tone::Warn;
		for (std::string &line : FirewallLogLines(v, port, exe))
			Say(warn, std::move(line));
	}

	void Start(const Job &job) {
		const uint32_t gen   = job.generation;
		const auto     stale = [this, gen] { return Stale(gen); };
		const auto    &local = job.local;
		ReachStatus    now;

		CheckFirewall(job.port, true);

		if (job.lookUp && !stale()) {
			std::string    why;
			const uint32_t addr = LookUpPublicAddress(stale, &why);
			if (Publish(gen, &now, [&](ReachStatus &s) {
				    s.lookup     = addr ? LookupState::Found : LookupState::Failed;
				    s.publicAddr = addr;
				    s.lookupWhy  = why;
			    })) {
				Say(!addr, LookupLogLine(now, job.port));
				if (addr && addr != DirectPublicAddress(local))
					Say(false, HostLogLine(local, job.port));
			}
		}

		if (job.openPort && !stale()) {
			ReachStatus found;
			if (RouterCanHelp(local))
				OpenPort(router, job.port, PreferredLanAddress(local), stale, &found);
			else
				found.port = DirectPublicAddress(local) ? PortState::NotNeeded : PortState::Failed;
			if (found.port == PortState::Failed && found.portWhy.empty() && !RouterCanHelp(local))
				found.portWhy = "this machine has no address on a home network";
			if (Publish(gen, &now, [&](ReachStatus &s) {
				    s.port       = found.port;
				    s.routerAddr = found.routerAddr;
				    s.takenBy    = found.takenBy;
				    s.portWhy    = found.portWhy;
			    }))
				Say(PortAdvice(local, now, job.port).tone == Tone::Warn,
				    PortLogLine(local, now, job.port));
		}
	}

	void Close() {
		if (!router.mappings)
			return;
		const uint16_t port  = router.port;
		const bool     owned = router.owned;
		const bool     ok    = ClosePort(router);
		if (!owned)
			return;
		char line[200];
		if (ok)
			std::snprintf(line, sizeof line, "closed port %u on the router again",
			              static_cast<unsigned>(port));
		else
			std::snprintf(line, sizeof line,
			              "could not close port %u on the router; it stays forwarded here until "
			              "the router forgets it",
			              static_cast<unsigned>(port));
		Say(!ok, line);
	}

	void Run() {
		const HRESULT com  = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		uint16_t      port = 0;
		for (;;) {
			Job  job;
			bool have = false;
			{
				std::unique_lock<std::mutex> lock(mutex);
				have = wake.wait_for(lock, std::chrono::seconds(10), [this] { return !jobs.empty(); });
				if (have) {
					job = std::move(jobs.front());
					jobs.pop_front();
				} else if (running && InTheWay(firewall.state)) {
					// Nothing asked, and the firewall is still in the way: look
					// again, since the host may be answering Windows' own prompt
					// or changing a rule by hand right now.
					have = true;
				}
			}
			if (!have)
				continue;
			if (job.kind == Job::Quit)
				break;
			switch (job.kind) {
			case Job::Begin:
				Close();   // only when Begin came twice with no End between
				port = job.port;
				Start(job);
				break;
			case Job::End:
				Close();
				break;
			default:   // Recheck
				if (port)
					CheckFirewall(port, job.announce);
				break;
			}
		}
		Close();
		if (SUCCEEDED(com))
			CoUninitialize();
		{
			std::lock_guard<std::mutex> lock(mutex);
			finished = true;
		}
		done.notify_all();
	}

	Router router;   // the thread's alone
};

ReachProbe::ReachProbe() : m_shared(std::make_shared<Shared>()) {
	std::shared_ptr<Shared> s = m_shared;
	m_shared->worker          = std::thread([s] { s->Run(); });
}

ReachProbe::~ReachProbe() { Finish(); }

void ReachProbe::Begin(uint16_t port, const std::vector<LocalAddress> &local, bool lookUp,
                       bool openPort) {
	Shared::Job job;
	job.kind     = Shared::Job::Begin;
	job.port     = port;
	job.local    = local;
	job.lookUp   = lookUp;
	job.openPort = openPort;
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		job.generation = ++m_shared->generation;
		m_shared->running = true;

		// What there is to show before anything has answered.
		ReachStatus &s = m_shared->status;
		s              = ReachStatus{};
		s.lookup       = lookUp ? LookupState::Pending : LookupState::Off;
		s.lan          = PreferredLanAddress(local);
		s.port         = !openPort                 ? PortState::Off
		                 : RouterCanHelp(local)    ? PortState::Pending
		                 : DirectPublicAddress(local) ? PortState::NotNeeded
		                                              : PortState::Failed;
		m_shared->firewall = FirewallVerdict{};
		m_shared->jobs.push_back(std::move(job));
	}
	m_shared->wake.notify_one();
}

void ReachProbe::End() {
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		if (!m_shared->running)
			return;
		++m_shared->generation;
		m_shared->running = false;
		Shared::Job job;
		job.kind = Shared::Job::End;
		m_shared->jobs.push_back(std::move(job));
	}
	m_shared->wake.notify_one();
}

ReachStatus ReachProbe::Status() const {
	std::lock_guard<std::mutex> lock(m_shared->mutex);
	return m_shared->status;
}

FirewallVerdict ReachProbe::Firewall() const {
	std::lock_guard<std::mutex> lock(m_shared->mutex);
	return m_shared->firewall;
}

std::vector<ProbeLine> ReachProbe::TakeLines() {
	std::lock_guard<std::mutex> lock(m_shared->mutex);
	std::vector<ProbeLine>      out;
	out.swap(m_shared->lines);
	return out;
}

bool ReachProbe::Allowing() const {
	std::lock_guard<std::mutex> lock(m_shared->mutex);
	return m_shared->allowing;
}

void ReachProbe::AllowInFirewall(void *hwnd) {
	FirewallState state;
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		if (m_shared->allowing)
			return;
		m_shared->allowing = true;
		state              = m_shared->firewall.state;
	}

	// A thread of its own, because the permission prompt waits for the host
	// for as long as the host likes, and neither the window nor the probe's
	// own work should wait with it.
	std::shared_ptr<Shared> s = m_shared;
	std::thread([s, hwnd, state] {
		const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		const ShellCommand command = FirewallFix(ExePath(), state == FirewallState::Blocked);
		const std::wstring file    = Widen(command.file);
		const std::wstring params  = Widen(command.params);

		SHELLEXECUTEINFOW info{};
		info.cbSize       = sizeof info;
		info.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
		info.hwnd         = static_cast<HWND>(hwnd);
		info.lpVerb       = L"runas";
		info.lpFile       = file.c_str();
		info.lpParameters = params.c_str();
		info.nShow        = SW_HIDE;
		char line[200];
		if (!ShellExecuteExW(&info)) {
			const DWORD error = GetLastError();
			if (error == ERROR_CANCELLED)
				s->Say(false, "the firewall was left as it was: Windows' permission prompt was "
				              "turned down");
			else {
				std::snprintf(line, sizeof line,
				              "could not ask Windows to change the firewall (error %lu)",
				              static_cast<unsigned long>(error));
				s->Say(true, line);
			}
		} else {
			DWORD code = 1;
			if (info.hProcess) {
				WaitForSingleObject(info.hProcess, 60000);
				GetExitCodeProcess(info.hProcess, &code);
				CloseHandle(info.hProcess);
			}
			if (code == 0) {
				s->Say(false, "added a Windows Firewall rule letting server.exe receive UDP");
			} else {
				std::snprintf(line, sizeof line,
				              "netsh could not change the firewall (it ended with %lu)",
				              static_cast<unsigned long>(code));
				s->Say(true, line);
			}
		}
		if (SUCCEEDED(com))
			CoUninitialize();
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			s->allowing = false;
		}
		Shared::Job job;
		job.kind     = Shared::Job::Recheck;
		job.announce = true;
		s->Queue(std::move(job));
	}).detach();
}

void ReachProbe::Finish(uint32_t timeoutMs) {
	if (!m_shared->worker.joinable())
		return;
	End();
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		// Whatever it is in the middle of is for a server that is going, so
		// it stops at the next chance; closing the port is not skipped.
		m_shared->quitting = true;
		Shared::Job job;
		job.kind = Shared::Job::Quit;
		m_shared->jobs.push_back(std::move(job));
	}
	m_shared->wake.notify_one();

	bool finished;
	{
		std::unique_lock<std::mutex> lock(m_shared->mutex);
		finished = m_shared->done.wait_for(lock, std::chrono::milliseconds(timeoutMs),
		                                   [this] { return m_shared->finished; });
	}
	if (finished)
		m_shared->worker.join();
	else
		m_shared->worker.detach();   // it holds its own reference to Shared
}

std::string ReachProbe::ExePath() {
	wchar_t     path[MAX_PATH * 2] = {0};
	const DWORD n                  = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
	return Narrow(path, static_cast<int>(n));
}

} // namespace coopiii
