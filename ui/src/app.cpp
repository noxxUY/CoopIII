#include "ui/app.h"

#include "ui/anim.h"
#include "ui/fonts.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include <d3d11.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <windows.h>
#include <windowsx.h>

#include <chrono>
#include <functional>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

namespace ui {
namespace {

constexpr wchar_t kClassName[] = L"CoopIIIWindow";

// The frame is only hidden, not removed, so the window keeps its shadow and
// Aero Snap. These are the widths of the invisible resize edges.
constexpr int kResizeBorder = 6;

// The timer that keeps App::Run's `always` callback going while Windows has
// the message loop - see WM_ENTERSIZEMOVE.
constexpr UINT_PTR kLoopTimer = 1;

App::Impl *FromHwnd(HWND hwnd);

} // namespace

struct App::Impl {
	HWND                    hwnd    = nullptr;
	ID3D11Device           *device  = nullptr;
	ID3D11DeviceContext    *context = nullptr;
	IDXGISwapChain         *swap    = nullptr;
	ID3D11RenderTargetView *rtv     = nullptr;

	AppOptions options;
	App       *owner = nullptr;

	float dpiScale      = 1.0f;
	float dragHeight    = 40.0f;
	float dragEndX      = 1e9f;
	bool  quit          = false;
	bool  occluded      = false;

	// The theme in force this frame. Not Dark() or Light() but a point
	// between them, so the switch in the title bar cross-fades the window
	// instead of blinking it. `themeMix` is 0 dark, 1 light.
	Theme theme    = Theme::Dark();
	float themeMix = 0.0f;

	// Run every turn of the loop, and from the timer below while Windows has
	// the loop. See App::Run.
	std::function<void()> always;

	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

	bool CreateDevice();
	void DestroyDevice();
	void CreateRenderTarget();
	void DestroyRenderTarget();
	void ApplyDpi(UINT dpi);

	LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

namespace {

App::Impl *FromHwnd(HWND hwnd) {
	return reinterpret_cast<App::Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_NCCREATE) {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lParam);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA,
		                  reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
	}
	if (App::Impl *impl = FromHwnd(hwnd))
		return impl->HandleMessage(hwnd, msg, wParam, lParam);
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::wstring Widen(const char *utf8) {
	if (!utf8 || !*utf8)
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
	std::wstring out(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
	return out;
}

std::string Narrow(const wchar_t *wide) {
	if (!wide || !*wide)
		return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
	std::string out(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), n, nullptr, nullptr);
	return out;
}

} // namespace

// ---- device ---------------------------------------------------------------

bool App::Impl::CreateDevice() {
	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferCount          = 2;
	desc.BufferDesc.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferDesc.RefreshRate.Numerator   = 60;
	desc.BufferDesc.RefreshRate.Denominator = 1;
	desc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = hwnd;
	desc.SampleDesc.Count = 1;
	desc.Windowed         = TRUE;
	desc.SwapEffect       = DXGI_SWAP_EFFECT_DISCARD;
	desc.Flags            = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL       got      = D3D_FEATURE_LEVEL_11_0;

	HRESULT hr = D3D11CreateDeviceAndSwapChain(
	    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 2, D3D11_SDK_VERSION, &desc,
	    &swap, &device, &got, &context);
	if (hr == DXGI_ERROR_UNSUPPORTED) {
		// No usable GPU: a software device still draws this UI fine, and it is
		// a lot better than refusing to start.
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, wanted,
		                                   2, D3D11_SDK_VERSION, &desc, &swap, &device, &got,
		                                   &context);
	}
	if (FAILED(hr))
		return false;

	CreateRenderTarget();
	return true;
}

void App::Impl::DestroyDevice() {
	DestroyRenderTarget();
	if (swap) {
		swap->Release();
		swap = nullptr;
	}
	if (context) {
		context->Release();
		context = nullptr;
	}
	if (device) {
		device->Release();
		device = nullptr;
	}
}

void App::Impl::CreateRenderTarget() {
	ID3D11Texture2D *back = nullptr;
	if (SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
		device->CreateRenderTargetView(back, nullptr, &rtv);
		back->Release();
	}
}

void App::Impl::DestroyRenderTarget() {
	if (rtv) {
		rtv->Release();
		rtv = nullptr;
	}
}

void App::Impl::ApplyDpi(UINT dpi) {
	dpiScale = static_cast<float>(dpi) / 96.0f;
	if (ImGui::GetCurrentContext())
		ImGui::GetStyle().FontScaleDpi = dpiScale;
}

// ---- messages -------------------------------------------------------------

LRESULT App::Impl::HandleMessage(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(wnd, msg, wParam, lParam))
		return true;

	switch (msg) {
	case WM_NCCALCSIZE:
		// Hide the frame without removing it. Returning 0 with wParam TRUE
		// makes the client area cover the whole window, shadow and snap
		// behaviour included.
		if (wParam == TRUE) {
			if (IsZoomed(wnd)) {
				// A maximised frameless window would otherwise hang its edges
				// over the neighbouring monitors by the frame's width.
				auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
				const int   cx = GetSystemMetrics(SM_CXSIZEFRAME) +
				               GetSystemMetrics(SM_CXPADDEDBORDER);
				const int cy = GetSystemMetrics(SM_CYSIZEFRAME) +
				               GetSystemMetrics(SM_CXPADDEDBORDER);
				params->rgrc[0].left += cx;
				params->rgrc[0].right -= cx;
				params->rgrc[0].top += cy;
				params->rgrc[0].bottom -= cy;
			}
			return 0;
		}
		break;

	case WM_NCHITTEST: {
		POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
		RECT  rc;
		GetWindowRect(wnd, &rc);

		if (options.resizable && !IsZoomed(wnd)) {
			const int border = static_cast<int>(kResizeBorder * dpiScale);
			const bool left   = pt.x < rc.left + border;
			const bool right  = pt.x >= rc.right - border;
			const bool top    = pt.y < rc.top + border;
			const bool bottom = pt.y >= rc.bottom - border;
			if (top && left)    return HTTOPLEFT;
			if (top && right)   return HTTOPRIGHT;
			if (bottom && left) return HTBOTTOMLEFT;
			if (bottom && right)return HTBOTTOMRIGHT;
			if (left)           return HTLEFT;
			if (right)          return HTRIGHT;
			if (top)            return HTTOP;
			if (bottom)         return HTBOTTOM;
		}

		// The title bar: everything along the top that the app has not put a
		// button on. dragEndX is set each frame by TitleBar().
		const float x = (pt.x - rc.left) / dpiScale;
		const float y = (pt.y - rc.top) / dpiScale;
		if (y < dragHeight && x < dragEndX)
			return HTCAPTION;
		return HTCLIENT;
	}

	// Dragging or resizing hands the message loop to Windows until the mouse
	// comes up, and App::Run does not get another turn in all that time. A
	// timer is the way back in: without it the server stops answering packets
	// for as long as somebody holds its title bar.
	case WM_ENTERSIZEMOVE:
		if (always)
			SetTimer(wnd, kLoopTimer, 16, nullptr);
		break;

	case WM_EXITSIZEMOVE:
		KillTimer(wnd, kLoopTimer);
		break;

	case WM_TIMER:
		if (wParam == kLoopTimer && always)
			always();
		return 0;

	case WM_DPICHANGED: {
		ApplyDpi(HIWORD(wParam));
		const RECT *suggested = reinterpret_cast<const RECT *>(lParam);
		SetWindowPos(wnd, nullptr, suggested->left, suggested->top,
		             suggested->right - suggested->left, suggested->bottom - suggested->top,
		             SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}

	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED && swap) {
			DestroyRenderTarget();
			swap->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
		}
		return 0;

	case WM_GETMINMAXINFO:
		if (!options.resizable) {
			auto *mmi = reinterpret_cast<MINMAXINFO *>(lParam);
			const LONG w = static_cast<LONG>(options.width * dpiScale);
			const LONG h = static_cast<LONG>(options.height * dpiScale);
			mmi->ptMinTrackSize = {w, h};
			mmi->ptMaxTrackSize = {w, h};
		}
		return 0;

	case WM_SYSCOMMAND:
		// Alt and F10 open the system menu over the UI; nothing here wants it.
		if ((wParam & 0xFFF0) == SC_KEYMENU)
			return 0;
		break;

	case WM_CLOSE:
		quit = true;
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}

	return DefWindowProcW(wnd, msg, wParam, lParam);
}

// ---- lifetime -------------------------------------------------------------

App::App(const AppOptions &options) {
	m_impl          = new Impl();
	m_impl->options = options;
	m_impl->owner   = this;
	m_impl->dragHeight = options.titleBarHeight;

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = GetModuleHandleW(nullptr);
	wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
	wc.lpszClassName = kClassName;
	wc.hIcon         = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
	wc.hIconSm       = wc.hIcon;
	RegisterClassExW(&wc);

	const UINT dpi = GetDpiForSystem();
	m_impl->ApplyDpi(dpi);

	DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
	if (!options.resizable)
		style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
	if (!options.maximizeBox)
		style &= ~WS_MAXIMIZEBOX;

	const int w = static_cast<int>(options.width * m_impl->dpiScale);
	const int h = static_cast<int>(options.height * m_impl->dpiScale);

	m_impl->hwnd = CreateWindowExW(0, kClassName, Widen(options.title).c_str(), style,
	                               CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
	                               wc.hInstance, m_impl);
	if (!m_impl->hwnd)
		return;

	// The frame was worked out while the window was being created, before
	// WM_NCCALCSIZE could say it should be empty. Ask for it again.
	SetWindowPos(m_impl->hwnd, nullptr, 0, 0, 0, 0,
	             SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
	                 SWP_NOACTIVATE);

	// Give the frameless window the system drop shadow back.
	const MARGINS shadow = {1, 1, 1, 1};
	DwmExtendFrameIntoClientArea(m_impl->hwnd, &shadow);

	// Tell the window manager which way the title bar is painted, so the
	// snap-layouts popup and the taskbar preview match.
	BOOL dark = !WindowsPrefersLight();
	DwmSetWindowAttribute(m_impl->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

	if (!m_impl->CreateDevice())
		return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename  = nullptr;   // nothing here is worth remembering between runs
	io.LogFilename  = nullptr;
	io.ConfigWindowsMoveFromTitleBarOnly = true;
	ImGui::GetStyle().FontScaleDpi = m_impl->dpiScale;

	ImGui_ImplWin32_Init(m_impl->hwnd);
	ImGui_ImplDX11_Init(m_impl->device, m_impl->context);
	Fonts::Load();
	LoadImages(m_impl->device);

	m_light    = WindowsPrefersLight();
	m_animates = SystemWantsAnimation();
	m_ok       = true;

	// Start on the theme rather than cross-fading into it: the first frame a
	// light-themed machine sees should not be a dark one.
	m_impl->themeMix = m_light ? 1.0f : 0.0f;
	m_impl->theme    = Theme::For(m_light);

	ShowWindow(m_impl->hwnd, SW_SHOW);
	UpdateWindow(m_impl->hwnd);
}

App::~App() {
	if (m_ok) {
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}
	if (m_impl) {
		m_impl->DestroyDevice();
		if (m_impl->hwnd)
			DestroyWindow(m_impl->hwnd);
		delete m_impl;
	}
	UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

int App::Run(const std::function<void()> &frame, const std::function<void()> &always) {
	if (!m_ok)
		return 1;

	// Held so the window procedure can reach it: a drag or a resize hands the
	// loop to Windows, and a timer is the only way back in while that lasts.
	m_impl->always = always;

	while (!m_impl->quit) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
				m_impl->quit = true;
		}
		if (m_impl->quit)
			break;

		if (always)
			always();

		if (IsIconic(m_impl->hwnd)) {
			Sleep(16);
			continue;
		}

		if (m_followOs) {
			const bool light = WindowsPrefersLight();
			if (light != m_light) {
				m_light    = light;
				BOOL dark  = !light;
				DwmSetWindowAttribute(m_impl->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
				                      sizeof(dark));
			}
		}

		RECT client;
		GetClientRect(m_impl->hwnd, &client);
		const float pxW = static_cast<float>(client.right - client.left);
		const float pxH = static_cast<float>(client.bottom - client.top);

		ImGuiIO &io = ImGui::GetIO();
		// Lay out in design pixels; render at the screen's real resolution.
		io.DisplaySize             = ImVec2(pxW / m_impl->dpiScale, pxH / m_impl->dpiScale);
		io.DisplayFramebufferScale = ImVec2(m_impl->dpiScale, m_impl->dpiScale);

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		// The Win32 backend measures the client rect in physical pixels;
		// ours is logical, so put it back after the backend has had its say.
		io.DisplaySize             = ImVec2(pxW / m_impl->dpiScale, pxH / m_impl->dpiScale);
		io.DisplayFramebufferScale = ImVec2(m_impl->dpiScale, m_impl->dpiScale);
		ImGui::NewFrame();

		// Everything that moves is stepped from here, once, before anything
		// asks what it looks like this frame.
		BeginMotionFrame(io.DeltaTime, m_animates);
		m_impl->themeMix = Approach(m_impl->themeMix, m_light ? 1.0f : 0.0f, 11.0f);
		m_impl->theme    = Theme::Blend(Theme::Dark(), Theme::Light(), m_impl->themeMix);

		frame();

		ImGui::Render();
		const Theme &theme = CurrentTheme();
		const ImVec4 clear = ImGui::ColorConvertU32ToFloat4(theme.bgWindow);
		const float  rgba[4] = {clear.x, clear.y, clear.z, 1.0f};
		m_impl->context->OMSetRenderTargets(1, &m_impl->rtv, nullptr);
		m_impl->context->ClearRenderTargetView(m_impl->rtv, rgba);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		// Vsync. Nothing here needs to run faster than the screen, and the
		// city map is the only thing moving.
		const HRESULT hr = m_impl->swap->Present(1, 0);
		m_impl->occluded = hr == DXGI_STATUS_OCCLUDED;
		if (m_impl->occluded)
			Sleep(16);
	}
	return 0;
}

// The blended theme, not Dark() or Light(): mid-switch the window is somewhere
// between the two, and every widget takes its colours from here.
const Theme &App::CurrentTheme() const { return m_impl->theme; }

void App::SetTheme(bool light) {
	m_light    = light;
	m_followOs = false;
	if (m_impl && m_impl->hwnd) {
		BOOL dark = !light;
		DwmSetWindowAttribute(m_impl->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
	}
}

void App::FollowWindowsTheme() {
	m_followOs = true;
	m_light    = WindowsPrefersLight();
}

float App::Seconds() const {
	using namespace std::chrono;
	return duration<float>(steady_clock::now() - m_impl->start).count();
}

void App::Minimize() { ShowWindow(m_impl->hwnd, SW_MINIMIZE); }

void App::ToggleMaximize() {
	ShowWindow(m_impl->hwnd, IsZoomed(m_impl->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

void App::Close() { m_impl->quit = true; }

bool App::IsWindowMaximized() const { return IsZoomed(m_impl->hwnd) != FALSE; }

void App::SetDragRegion(float height, float dragEndX) {
	m_impl->dragHeight = height;
	m_impl->dragEndX   = dragEndX;
}

void *App::Hwnd() const { return m_impl ? m_impl->hwnd : nullptr; }

std::string App::PickFolder(const char *title, const std::string &startAt) {
	std::string chosen;
	if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
		return chosen;

	IFileOpenDialog *dialog = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
	                               IID_PPV_ARGS(&dialog))) &&
	    dialog) {
		DWORD flags = 0;
		dialog->GetOptions(&flags);
		dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		dialog->SetTitle(Widen(title).c_str());

		if (!startAt.empty()) {
			IShellItem *item = nullptr;
			if (SUCCEEDED(SHCreateItemFromParsingName(Widen(startAt.c_str()).c_str(), nullptr,
			                                          IID_PPV_ARGS(&item))) &&
			    item) {
				dialog->SetFolder(item);
				item->Release();
			}
		}

		if (SUCCEEDED(dialog->Show(nullptr))) {
			IShellItem *item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
					chosen = Narrow(path);
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dialog->Release();
	}

	CoUninitialize();
	return chosen;
}

void App::SetClipboard(const std::string &text) {
	if (!OpenClipboard(nullptr))
		return;
	EmptyClipboard();
	const std::wstring wide = Widen(text.c_str());
	const size_t       bytes = (wide.size() + 1) * sizeof(wchar_t);
	if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
		if (void *dst = GlobalLock(mem)) {
			memcpy(dst, wide.c_str(), bytes);
			GlobalUnlock(mem);
			SetClipboardData(CF_UNICODETEXT, mem);
		} else {
			GlobalFree(mem);
		}
	}
	CloseClipboard();
}

} // namespace ui
