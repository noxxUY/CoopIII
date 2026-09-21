// Tests the detour engine by hooking real functions in this process.
//   xmake build hooktest && xmake run hooktest
//
// About as close as we can get to the real thing without GTA III running.
// MinHook is rewriting actual instructions here, so a broken trampoline
// fails loudly instead of quietly.

#include "hook/hook.h"

#include <cstdio>

using namespace coopiii;

namespace {

int g_failures = 0;

void Check(bool cond, const char *what) {
	std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
	if (!cond)
		++g_failures;
}

// Hook targets. noinline plus a non-trivial body, so the optimiser leaves a
// real function with enough prologue bytes for a 5-byte relative jump.
volatile int g_sink = 0;

__declspec(noinline) int TargetAdd(int a, int b) {
	g_sink += a;
	return a + b;
}

__declspec(noinline) int TargetMul(int a, int b) {
	g_sink += b;
	return a * b;
}

Detour g_addDetour;
Detour g_mulDetour;

int  g_addCalls = 0;
bool g_callOriginalFromHook = true;

int HookedAdd(int a, int b) {
	++g_addCalls;
	if (!g_callOriginalFromHook)
		return -1;
	// Reaching the real function through the trampoline is the part that
	// actually proves the detour works.
	return g_addDetour.Original<int (*)(int, int)>()(a, b) * 10;
}

int HookedMul(int a, int b) {
	return g_mulDetour.Original<int (*)(int, int)>()(a, b) + 1;
}

void TestBasicDetour() {
	std::printf("\nbasic detour\n");

	Check(TargetAdd(2, 3) == 5, "function behaves normally before hooking");

	Check(g_addDetour.Install("TargetAdd", reinterpret_cast<void *>(&TargetAdd),
	                          reinterpret_cast<void *>(&HookedAdd)),
	      "hook installs");
	Check(g_addDetour.IsInstalled(), "reports installed");

	g_addCalls = 0;
	const int result = TargetAdd(2, 3);
	Check(g_addCalls == 1, "call is diverted to the hook");
	Check(result == 50, "trampoline reaches the original (5 * 10)");

	g_addDetour.Remove();
	Check(!g_addDetour.IsInstalled(), "reports removed");
	Check(TargetAdd(2, 3) == 5, "original behaviour restored after removal");

	g_addCalls = 0;
	TargetAdd(1, 1);
	Check(g_addCalls == 0, "hook no longer runs after removal");
}

void TestTrampolineOnly() {
	std::printf("\ntrampoline\n");
	Check(g_mulDetour.Install("TargetMul", reinterpret_cast<void *>(&TargetMul),
	                          reinterpret_cast<void *>(&HookedMul)),
	      "second hook installs alongside");
	Check(TargetMul(3, 4) == 13, "trampoline returns the original result (12 + 1)");
	g_mulDetour.Remove();
	Check(TargetMul(3, 4) == 12, "restored");
}

void TestHookWithoutCallingOriginal() {
	std::printf("\nreplacement without original\n");
	Check(g_addDetour.Install("TargetAdd", reinterpret_cast<void *>(&TargetAdd),
	                          reinterpret_cast<void *>(&HookedAdd)),
	      "hook reinstalls after a previous removal");

	g_callOriginalFromHook = false;
	Check(TargetAdd(7, 7) == -1, "hook can fully replace the function");
	g_callOriginalFromHook = true;

	g_addDetour.Remove();
}

void TestFailuresAreRecorded() {
	std::printf("\nfailure reporting\n");
	ClearHookFailures();

	Detour bad;
	Check(!bad.Install("NullTarget", nullptr, reinterpret_cast<void *>(&HookedAdd)),
	      "hooking a null target fails");
	Check(!bad.IsInstalled(), "failed hook is not marked installed");
	Check(HookFailures().size() == 1, "failure is recorded");
	if (HookFailures().size() == 1) {
		Check(HookFailures()[0].name == std::string("NullTarget"), "failure names the hook");
		Check(!HookFailures()[0].reason.empty(), "failure has a reason");
	}

	ClearHookFailures();
	Detour badPattern;
	Check(!badPattern.InstallByPattern("Absent", "DE AD BE EF C0 FF EE 11 22 33 44 55",
	                                   reinterpret_cast<void *>(&HookedAdd)),
	      "hooking an unfound pattern fails");
	Check(HookFailures().size() == 1, "scan failure is recorded");
	if (HookFailures().size() == 1)
		Check(HookFailures()[0].reason.find("not found") != std::string::npos,
		      "reason explains the scan result");

	ClearHookFailures();
}

void TestRaii() {
	std::printf("\nRAII\n");
	g_addCalls = 0;
	{
		Detour scoped;
		Check(scoped.Install("scoped", reinterpret_cast<void *>(&TargetMul),
		                     reinterpret_cast<void *>(&HookedMul)),
		      "scoped hook installs");
	}
	// If the destructor didn't unhook, HookedMul would still be running and
	// this would come back 13.
	Check(TargetMul(3, 4) == 12, "destructor removed the hook");
}

void TestScanModuleFindsRealCode() {
	std::printf("\nscanning this module\n");
	const uint8_t *begin = nullptr;
	size_t         size  = 0;
	Check(GetMainModuleCode(&begin, &size), "locates this module's code section");
	Check(size > 0x1000, "code section has a sane size");

	// TargetAdd must live inside the range we just computed.
	const auto *fn = reinterpret_cast<const uint8_t *>(&TargetAdd);
	Check(fn >= begin && fn < begin + size, "a known function lies inside the range");
}

} // namespace

int main() {
	if (!HookInit()) {
		std::printf("MinHook failed to initialise\n");
		for (const auto &f : HookFailures())
			std::printf("  %s: %s\n", f.name.c_str(), f.reason.c_str());
		return 1;
	}

	TestBasicDetour();
	TestTrampolineOnly();
	TestHookWithoutCallingOriginal();
	TestFailuresAreRecorded();
	TestRaii();
	TestScanModuleFindsRealCode();

	HookShutdown();

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
	            g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
