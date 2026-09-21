#include "hook.h"

#include <MinHook.h>

#include <utility>

namespace coopiii {

namespace {

int                      g_initRefs = 0;
std::vector<HookFailure> g_failures;

const char *MhText(MH_STATUS s) {
	const char *t = MH_StatusToString(s);
	return t ? t : "unknown MinHook error";
}

} // namespace

bool HookInit() {
	if (g_initRefs++ > 0)
		return true;
	const MH_STATUS s = MH_Initialize();
	if (s != MH_OK) {
		g_initRefs = 0;
		RecordHookFailure("MinHook", MhText(s));
		return false;
	}
	return true;
}

void HookShutdown() {
	if (g_initRefs > 0 && --g_initRefs == 0)
		MH_Uninitialize();
}

Detour::~Detour() {
	Remove();
}

Detour::Detour(Detour &&other) noexcept
    : m_name(other.m_name), m_target(other.m_target),
      m_trampoline(other.m_trampoline), m_installed(other.m_installed) {
	other.m_target     = nullptr;
	other.m_trampoline = nullptr;
	other.m_installed  = false;
}

Detour &Detour::operator=(Detour &&other) noexcept {
	if (this != &other) {
		Remove();
		m_name       = other.m_name;
		m_target     = other.m_target;
		m_trampoline = other.m_trampoline;
		m_installed  = other.m_installed;
		other.m_target     = nullptr;
		other.m_trampoline = nullptr;
		other.m_installed  = false;
	}
	return *this;
}

bool Detour::Install(const char *name, void *target, void *replacement) {
	Remove();

	m_name = name ? name : "";
	if (!target) {
		RecordHookFailure(m_name, "target address is null");
		return false;
	}

	MH_STATUS s = MH_CreateHook(target, replacement, &m_trampoline);
	if (s != MH_OK) {
		RecordHookFailure(m_name, std::string("MH_CreateHook: ") + MhText(s));
		m_trampoline = nullptr;
		return false;
	}

	s = MH_EnableHook(target);
	if (s != MH_OK) {
		RecordHookFailure(m_name, std::string("MH_EnableHook: ") + MhText(s));
		MH_RemoveHook(target);
		m_trampoline = nullptr;
		return false;
	}

	m_target    = target;
	m_installed = true;
	return true;
}

bool Detour::InstallByPattern(const char *name, const char *idaPattern, void *replacement,
                              ptrdiff_t patternOffset) {
	const ScanResult scan = ScanModule(idaPattern);
	if (!scan.Ok()) {
		RecordHookFailure(name ? name : "", DescribeScan(scan));
		return false;
	}
	return Install(name, scan.address + patternOffset, replacement);
}

void Detour::Remove() {
	if (!m_installed || !m_target)
		return;

	MH_DisableHook(m_target);
	MH_RemoveHook(m_target);

	m_target     = nullptr;
	m_trampoline = nullptr;
	m_installed  = false;
}

const std::vector<HookFailure> &HookFailures() {
	return g_failures;
}

void ClearHookFailures() {
	g_failures.clear();
}

void RecordHookFailure(const char *name, const std::string &reason) {
	g_failures.push_back({name ? name : "", reason});
}

std::string DescribeScan(const ScanResult &result) {
	std::string out = std::string("pattern scan ") + result.StatusName();
	if (result.status == ScanResult::AMBIGUOUS)
		out += " (" + std::to_string(result.matches) + " matches, pattern isn't unique)";
	return out;
}

} // namespace coopiii
