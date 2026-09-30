// CoopIII.log, written next to the .asi.
//
// docs/compat.md §2.5: the target install runs an ASI loader, SilentPatch,
// Mod Loader, CLEO and four more plugins, all patching the same binary. When
// CoopIII doesn't work, this log is usually the only thing that says why -
// there's no console, and popping a message box inside a game hook is its
// own bug waiting to happen.
//
// Every line is flushed immediately. A buffered log is empty exactly when
// you need it most: after a crash.
//
// The log file opens denying other writers, and falls back to a per-pid name
// if that fails. Running two copies of the game out of one folder, which is
// how a co-op mod gets tested at all, would otherwise leave both processes
// truncating and interleaving lines into one CoopIII.log, which
// reads like one confused process instead of two sane ones. Call LogPath()
// to see which file this process actually got.
#pragma once

#include <cstdint>
#include <string>

namespace coopiii {

// Opens `path` for writing, denying other writers. If another process
// already holds it, opens LogPathForProcess(path, this pid) instead. Either
// way, LogPath() afterward reports what actually got opened - empty string
// if nothing did.
void LogOpen(const std::string &path);
void LogClose();

// The file Log() is currently writing to, or "" if none.
std::string LogPath();

// Inserts ".<pid>" before the extension: "C:\game\CoopIII.log" with pid 4312
// becomes "C:\game\CoopIII.4312.log". No extension just gets the pid tacked
// on the end. Exposed for tests since getting directory separators and
// dotted directory names right here is easy to mess up.
std::string LogPathForProcess(const std::string &path, uint32_t pid);

// Thread-safe. Safe to call before LogOpen too - those lines still make it
// to the debugger.
void Log(const char *fmt, ...);

} // namespace coopiii
