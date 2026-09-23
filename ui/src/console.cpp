#include "ui/console.h"

#include <windows.h>

#include <cstdio>

namespace ui {
namespace {

// Whether a standard handle already leads somewhere that is not a console -
// a file or a pipe, which is what `server --nogui > server.log` and
// `server --nogui | find "joined"` look like from in here. The CRT wires
// those up at startup whatever the subsystem is, so they need no help and
// must not be taken away.
bool Redirected(DWORD which) {
	const HANDLE handle = GetStdHandle(which);
	if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
		return false;
	const DWORD type = GetFileType(handle);
	return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

} // namespace

// A Windows-subsystem process is not attached to the console it was launched
// from, so its inherited console handles do not work until it asks to be.
bool OpenConsole(const wchar_t *title) {
	const bool outIsFile = Redirected(STD_OUTPUT_HANDLE);
	const bool errIsFile = Redirected(STD_ERROR_HANDLE);

	bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
	// A window of its own, but only when there is nothing else: allocating one
	// for output that is already going to a file would pop an empty black
	// rectangle and leave it there.
	bool allocated = false;
	if (!attached && !outIsFile)
		attached = allocated = AllocConsole() != FALSE;

	if (attached) {
		FILE *unused = nullptr;
		if (!outIsFile)
			freopen_s(&unused, "CONOUT$", "w", stdout);
		if (!errIsFile)
			freopen_s(&unused, "CONOUT$", "w", stderr);
		freopen_s(&unused, "CONIN$", "r", stdin);
		// Only a console of our own gets renamed. The one somebody launched
		// us from is theirs, and its title outlives this process.
		if (allocated)
			SetConsoleTitleW(title);
	}
	return attached || outIsFile;
}

} // namespace ui
