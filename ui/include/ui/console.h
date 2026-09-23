// A console for a Windows-subsystem executable that was asked not to open its
// window.
//
// server.exe and coopiii-launcher.exe are linked as Windows applications,
// because the window is the common case and a console subsystem flashes a
// black rectangle every time somebody double-clicks one. --nogui therefore has
// to go and find a console: the one it was launched from if there is one, a
// new one if there is not.
//
// The cost of that trade is visible and worth knowing about: cmd.exe does not
// wait for a Windows-subsystem process, so it prints its prompt straight away
// and the output arrives underneath it. `start /wait` makes it wait.
#pragma once

namespace ui {

// Points stdout, stderr and stdin at a console, leaving alone any of them that
// was redirected to a file or a pipe. `title` names a console this creates.
// Returns false when there is nowhere to write at all.
bool OpenConsole(const wchar_t *title);

} // namespace ui
