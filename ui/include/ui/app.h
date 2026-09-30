// The window every CoopIII GUI runs in.
//
// A frameless Win32 window with a Direct3D 11 swap chain and Dear ImGui on
// top. Frameless the good way: the frame is still there, WM_NCCALCSIZE just
// hides it, so the window keeps its drop shadow, Aero Snap, Win+Arrow and
// double-click-to-maximise while the title bar is ours to draw
// (design/DESIGN.md §6).
//
// Everything above this line lays out in the design's own pixels. The window
// is created at designSize x dpi and ImGui is told the logical size, so a
// value copied out of a .dc.html means the same thing on a 4K screen as it
// does on a 1080p one.
#pragma once

#include <functional>
#include <string>

#include <imgui.h>

namespace ui {

struct Theme;

struct AppOptions {
	const char *title      = "CoopIII";
	int         width      = 1040;   // design pixels
	int         height     = 700;
	bool        resizable  = false;
	bool        maximizeBox = false;
	// How tall the draggable strip at the top is. Matches the title bar the
	// app draws (DESIGN §3: 40 px).
	float       titleBarHeight = 40.0f;
};

class App {
public:
	explicit App(const AppOptions &options);
	~App();

	App(const App &)            = delete;
	App &operator=(const App &) = delete;

	bool Ok() const { return m_ok; }

	// Runs until the window closes. `frame` is called once per frame with a
	// full-window ImGui window already begun.
	//
	// `always` is called every turn of the loop whether or not a frame is
	// drawn - minimised, occluded, and while Windows has the loop inside its
	// own modal one because somebody is dragging the window. The server needs
	// this: its session lives in this process and has to keep answering
	// packets when nobody is looking at the window, which is most of the time
	// a dedicated server is up. Keep it short; it runs on the UI thread.
	int Run(const std::function<void()> &frame, const std::function<void()> &always = nullptr);

	// The theme in force. Follows Windows unless SetTheme has been called.
	const Theme &CurrentTheme() const;
	void         SetTheme(bool light);
	void         FollowWindowsTheme();
	bool         IsLight() const { return m_light; }

	// False when the OS has asked for no animation, so the city map holds
	// still - DESIGN §5.
	bool Animates() const { return m_animates; }

	// Seconds since the app started, for the city map's drift.
	float Seconds() const;

	// Title bar actions, wired to the buttons the app draws.
	void Minimize();
	void ToggleMaximize();
	void Close();
	bool IsWindowMaximized() const;

	// Rectangle the window manager should treat as the title bar. Set once
	// per frame by TitleBar(); everything left of `dragEndX` and above
	// `titleBarHeight` drags the window.
	void SetDragRegion(float height, float dragEndX);

	// A modal file-picker for a folder. Empty if the player cancelled.
	static std::string PickFolder(const char *title, const std::string &startAt);

	// Puts `text` on the clipboard.
	static void SetClipboard(const std::string &text);

	void *Hwnd() const;

	// Named here so the window procedure can reach it.
	struct Impl;

private:
	Impl *m_impl = nullptr;
	bool  m_ok   = false;
	bool  m_light    = false;
	bool  m_followOs = true;
	bool  m_animates = true;
};

} // namespace ui
