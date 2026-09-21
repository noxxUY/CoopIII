// Function detours, over MinHook.
//
// A failed hook is never silent here. The game keeps running, but the
// failure gets recorded and reported, because "multiplayer quietly did
// nothing" is a much worse bug to chase down than "CoopIII refused to start
// and said why".
#pragma once

#include "pattern.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coopiii {

// Process-wide MinHook init/teardown, refcounted.
bool HookInit();
void HookShutdown();

class Detour {
public:
	Detour() = default;
	~Detour();
	Detour(const Detour &) = delete;
	Detour &operator=(const Detour &) = delete;
	Detour(Detour &&other) noexcept;
	Detour &operator=(Detour &&other) noexcept;

	// Installs and enables a detour. `name` is only used in diagnostics.
	bool Install(const char *name, void *target, void *replacement);

	// Resolve a pattern and hook whatever it points at. `patternOffset` gets
	// added to the match before hooking, for patterns that anchor on
	// something near the function rather than its first byte.
	bool InstallByPattern(const char *name, const char *idaPattern, void *replacement,
	                      ptrdiff_t patternOffset = 0);

	void Remove();

	bool  IsInstalled() const { return m_installed; }
	void *Target() const { return m_target; }

	// The trampoline: call this to reach the original function.
	template <class Fn>
	Fn Original() const {
		return reinterpret_cast<Fn>(m_trampoline);
	}

private:
	const char *m_name       = "";
	void       *m_target     = nullptr;
	void       *m_trampoline = nullptr;
	bool        m_installed  = false;
};

// Why a hook or scan failed, kept around for the diagnostics the client
// prints on startup. None of this is fatal by itself; that's up to the caller.
struct HookFailure {
	std::string name;
	std::string reason;
};

const std::vector<HookFailure> &HookFailures();
void ClearHookFailures();
void RecordHookFailure(const char *name, const std::string &reason);

// Formats a ScanResult as a human-readable reason, for RecordHookFailure.
std::string DescribeScan(const ScanResult &result);

} // namespace coopiii
