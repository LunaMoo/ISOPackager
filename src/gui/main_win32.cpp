// Win32 + Direct3D 11 platform layer for Windows desktop: native file dialogs, drag and drop
// from Explorer, per-monitor DPI and UTF-8 paths throughout.

#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "core/sysinfo.h"
#include "core/util.h"
#include "gui/app.h"

using namespace isopk;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_context = nullptr;
IDXGISwapChain *g_swapChain = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
bool g_occluded = false;
UINT g_resizeW = 0, g_resizeH = 0;
HWND g_hwnd = nullptr;
gui::App *g_app = nullptr;
std::vector<std::string> g_dropped;
bool g_closeRequested = false;
float g_newDpiScale = 0;

std::string ToUtf8(const wchar_t *w) {
	if (!w)
		return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring ToWide(const std::string &s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

void CreateRenderTarget() {
	ID3D11Texture2D *back = nullptr;
	g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
	if (back) {
		g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
		back->Release();
	}
}

void CleanupRenderTarget() {
	if (g_rtv) {
		g_rtv->Release();
		g_rtv = nullptr;
	}
}

bool CreateDeviceD3D(HWND hwnd) {
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 2;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
	D3D_FEATURE_LEVEL got;
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain,
		&g_device, &got, &g_context);
	if (hr == DXGI_ERROR_UNSUPPORTED)  // no GPU driver (VMs, RDP): the WARP software rasteriser
		hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device,
			&got, &g_context);
	if (hr != S_OK)
		return false;
	CreateRenderTarget();
	return true;
}

void CleanupDeviceD3D() {
	CleanupRenderTarget();
	if (g_swapChain) g_swapChain->Release();
	if (g_context) g_context->Release();
	if (g_device) g_device->Release();
	g_swapChain = nullptr;
	g_context = nullptr;
	g_device = nullptr;
}

// IFileOpenDialog: files (multi-select, disc image filter) or a folder.
bool ShowOpenDialog(bool folder, std::vector<std::string> *out) {
	IFileOpenDialog *dlg = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
		return false;
	DWORD opts = 0;
	dlg->GetOptions(&opts);
	opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
	if (folder)
		opts |= FOS_PICKFOLDERS;
	else
		opts |= FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST;
	dlg->SetOptions(opts);
	if (!folder) {
		const COMDLG_FILTERSPEC filters[] = { { L"Disc images", L"*.iso;*.cso;*.zso;*.dax;*.chd;*.pbp" }, { L"All files", L"*.*" } };
		dlg->SetFileTypes(2, filters);
		dlg->SetTitle(L"Add disc images");
	} else {
		dlg->SetTitle(L"Choose a folder");
	}
	HRESULT hr = dlg->Show(g_hwnd);
	if (SUCCEEDED(hr)) {
		IShellItemArray *items = nullptr;
		if (SUCCEEDED(dlg->GetResults(&items))) {
			DWORD count = 0;
			items->GetCount(&count);
			for (DWORD i = 0; i < count; ++i) {
				IShellItem *item = nullptr;
				if (SUCCEEDED(items->GetItemAt(i, &item))) {
					PWSTR path = nullptr;
					if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
						out->push_back(ToUtf8(path));
						CoTaskMemFree(path);
					}
					item->Release();
				}
			}
			items->Release();
		}
	}
	dlg->Release();
	return true;  // cancelled counts as handled (no built-in browser fallback)
}

void Reveal(const std::string &path) {
	std::wstring w = ToWide(path);
	DWORD attr = GetFileAttributesW(w.c_str());
	if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
		ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return;
	}
	PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w.c_str());
	if (pidl) {
		SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
		ILFree(pidl);
	}
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
		return true;
	switch (msg) {
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) {
			g_resizeW = LOWORD(lParam);
			g_resizeH = HIWORD(lParam);
		}
		return 0;
	case WM_GETMINMAXINFO: {
		MINMAXINFO *mm = (MINMAXINFO *)lParam;
		mm->ptMinTrackSize.x = 640;
		mm->ptMinTrackSize.y = 420;
		return 0;
	}
	case WM_DPICHANGED: {
		g_newDpiScale = HIWORD(wParam) / 96.0f;
		const RECT *r = (const RECT *)lParam;
		SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_DROPFILES: {
		HDROP drop = (HDROP)wParam;
		UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		for (UINT i = 0; i < n; ++i) {
			UINT len = DragQueryFileW(drop, i, nullptr, 0);
			std::wstring w(len + 1, L'\0');
			DragQueryFileW(drop, i, w.data(), len + 1);
			w.resize(len);
			g_dropped.push_back(ToUtf8(w.c_str()));
		}
		DragFinish(drop);
		return 0;
	}
	case WM_CLOSE:
		if (!g_app || g_app->RequestClose())
			DestroyWindow(hwnd);
		return 0;
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU)
			return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	ImGui_ImplWin32_EnableDpiAwareness();
	float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr,
		nullptr, L"ISOPackager", LoadIconW(inst, MAKEINTRESOURCEW(1)) };
	RegisterClassExW(&wc);
	g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"ISOPackager", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
		(int)(1180 * scale), (int)(760 * scale), nullptr, nullptr, inst, nullptr);
	// Dark title bar to match the default theme (Windows 10 20H1+; harmless elsewhere).
	BOOL dark = TRUE;
	DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

	if (!CreateDeviceD3D(g_hwnd)) {
		CleanupDeviceD3D();
		MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"ISOPackager", MB_ICONERROR);
		return 1;
	}
	ShowWindow(g_hwnd, SW_SHOWDEFAULT);
	UpdateWindow(g_hwnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	static std::string iniPath = PathU8(U8Path(AppDataDir()) / "imgui.ini");
	{
		std::error_code ec;
		fs::create_directories(U8Path(AppDataDir()), ec);
	}
	io.IniFilename = iniPath.c_str();

	// Segoe UI covers Latin, Greek and Cyrillic file names; the bitmap font is the fallback.
	wchar_t winDir[MAX_PATH] = L"C:\\Windows";
	GetWindowsDirectoryW(winDir, MAX_PATH);
	std::string fontPath = ToUtf8(winDir) + "\\Fonts\\segoeui.ttf";
	bool fontLoaded = GetFileAttributesW(ToWide(fontPath).c_str()) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 17.0f);
	if (!fontLoaded)
		io.Fonts->AddFontDefault();
	ImGui::GetStyle().FontSizeBase = fontLoaded ? 17.0f : 13.0f;

	ImGui_ImplWin32_Init(g_hwnd);
	ImGui_ImplDX11_Init(g_device, g_context);

	gui::PlatformHooks hooks;
	hooks.dpiScale = scale;
	hooks.platformName = PlatformName();
	hooks.pickFiles = [](std::vector<std::string> *out) { return ShowOpenDialog(false, out); };
	hooks.pickFolder = [](std::string *out) {
		std::vector<std::string> r;
		if (!ShowOpenDialog(true, &r))
			return false;
		*out = r.empty() ? std::string() : r[0];
		return true;
	};
	hooks.reveal = Reveal;
	hooks.openUrl = [](const std::string &url) { ShellExecuteW(nullptr, L"open", ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL); };

	{
		gui::App app(hooks);
		g_app = &app;
		int argc = 0;
		LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		std::vector<std::string> startPaths;
		for (int i = 1; argv && i < argc; ++i)
			if (argv[i][0] != L'-')
				startPaths.push_back(ToUtf8(argv[i]));
		LocalFree(argv);
		app.AddPaths(startPaths);

		bool done = false;
		int idleFrames = 0;
		while (!done) {
			// Sleep while idle (no input, nothing running), so the app costs no CPU.
			if (!app.Animating() && idleFrames >= 3)
				MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
				if (msg.message == WM_QUIT)
					done = true;
				if (msg.message != WM_TIMER)
					idleFrames = 0;
			}
			if (done)
				break;
			if (!g_dropped.empty()) {
				app.AddPaths(g_dropped);
				g_dropped.clear();
			}
			if (g_newDpiScale > 0) {
				app.SetDpiScale(g_newDpiScale);
				g_newDpiScale = 0;
			}
			if (app.QuitConfirmed()) {
				DestroyWindow(g_hwnd);
				continue;
			}
			if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
				Sleep(20);
				continue;
			}
			g_occluded = false;
			if (g_resizeW != 0 && g_resizeH != 0) {
				CleanupRenderTarget();
				g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
				g_resizeW = g_resizeH = 0;
				CreateRenderTarget();
			}
			++idleFrames;

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
			app.Frame();
			ImGui::Render();
			const float clear[4] = { 0.08f, 0.09f, 0.10f, 1.0f };
			g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
			g_context->ClearRenderTargetView(g_rtv, clear);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			HRESULT hr = g_swapChain->Present(1, 0);
			g_occluded = hr == DXGI_STATUS_OCCLUDED;
		}
		g_app = nullptr;
	}

	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	CleanupDeviceD3D();
	DestroyWindow(g_hwnd);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);
	CoUninitialize();
	return 0;
}
