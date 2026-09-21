#include "log.h"

#include "clock.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#ifdef _WIN32
#include <share.h>
#include <windows.h>
#endif

namespace coopiii {

namespace {

std::mutex  g_mutex;
FILE       *g_file = nullptr;
std::string g_path;

// Opens for writing and denies other *writers* only - readers are fine, so
// you can still tail the file while the game runs. That's the whole point.
//
// This is also what makes the per-pid fallback work: a second copy of the
// game can't take CoopIII.log, so it falls back to CoopIII.<pid>.log. Plain
// fopen(path, "w") would let both processes succeed and both truncate, and
// the two line streams would interleave into something unreadable.
FILE *OpenDenyingWriters(const std::string &path) {
#ifdef _WIN32
	return _fsopen(path.c_str(), "w", _SH_DENYWR);
#else
	return std::fopen(path.c_str(), "w");
#endif
}

} // namespace

std::string LogPathForProcess(const std::string &path, uint32_t pid) {
	char suffix[16];
	std::snprintf(suffix, sizeof(suffix), ".%u", pid);

	// Only look for an extension inside the file name itself. "C:\my.stuff\CoopIII"
	// has a dot in it but no extension, and appending the pid before that dot
	// would put the suffix inside a directory name that doesn't exist.
	const size_t slash = path.find_last_of("\\/");
	const size_t dot   = path.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return path + suffix;

	return path.substr(0, dot) + suffix + path.substr(dot);
}

void LogOpen(const std::string &path) {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file)
		return;

	g_file = OpenDenyingWriters(path);
	if (g_file) {
		g_path = path;
	} else {
#ifdef _WIN32
		const uint32_t pid = GetCurrentProcessId();
#else
		const uint32_t pid = 0;
#endif
		const std::string alt = LogPathForProcess(path, pid);
		g_file                = OpenDenyingWriters(alt);
		g_path                = g_file ? alt : std::string();
	}

	if (g_file)
		std::setvbuf(g_file, nullptr, _IOLBF, 1024);
}

void LogClose() {
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file) {
		std::fclose(g_file);
		g_file = nullptr;
	}
	g_path.clear();
}

std::string LogPath() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_path;
}

void Log(const char *fmt, ...) {
	char body[1024];

	va_list args;
	va_start(args, fmt);
	std::vsnprintf(body, sizeof(body), fmt, args);
	va_end(args);

	char line[1100];
	std::snprintf(line, sizeof(line), "[%8u] %s\n", WallClock::NowMs(), body);

	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file) {
		std::fputs(line, g_file);
		std::fflush(g_file);
	}
#ifdef _WIN32
	// Also goes to the debugger, so the log can be watched live without
	// holding the file open, and so anything logged before LogOpen isn't lost.
	OutputDebugStringA(line);
#endif
}

} // namespace coopiii
