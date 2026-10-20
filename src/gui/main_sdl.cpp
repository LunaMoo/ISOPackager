// SDL2 + SDL_Renderer platform layer: Linux, macOS, FreeBSD, Android, iOS and UWP.
// (Windows desktop uses main_win32.cpp: Win32 + Direct3D 11, with native file dialogs.)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "core/sysinfo.h"
#include "core/util.h"
#include "gui/app.h"

#if defined(__ANDROID__) || (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || defined(__WINRT__)
#define ISOPK_TOUCH 1
#else
#define ISOPK_TOUCH 0
#endif
// Phones and tablets: these start in the mobile layout. UWP is touch-friendly but mostly runs on
// PCs and Xbox, so it starts in the desktop layout like every other platform.
#if defined(__ANDROID__) || (defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define ISOPK_MOBILE_PLATFORM 1
#else
#define ISOPK_MOBILE_PLATFORM 0
#endif

using namespace isopk;

#if !ISOPK_TOUCH && !defined(_WIN32)
// Desktop Unix: run a command without a shell (paths may contain anything).
static void Spawn(const std::vector<std::string> &argv) {
	std::string cmd;
	for (const std::string &a : argv) {
		std::string q = "'";
		for (char c : a)
			q += c == '\'' ? std::string("'\\''") : std::string(1, c);
		cmd += q + "' ";
	}
	cmd += ">/dev/null 2>&1 &";
	int rc = system(cmd.c_str());
	(void)rc;
}

// zenity / kdialog when present: real file dialogs on Linux and FreeBSD desktops.
static bool RunDialog(const std::string &cmd, std::vector<std::string> *out) {
	FILE *p = popen(cmd.c_str(), "r");
	if (!p)
		return false;
	std::string data;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), p)) > 0)
		data.append(buf, n);
	int rc = pclose(p);
	if (rc != 0 && data.empty())
		return rc != 127 * 256 && rc != 127;  // 127: tool missing -> not handled
	for (const std::string &line : Split(data, '\n'))
		out->push_back(line);
	return true;
}

static bool HaveTool(const char *name) {
	std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
	return system(cmd.c_str()) == 0;
}
#endif

int main(int argc, char **argv) {
	SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	// Mobile: settings and results go to the app's own folder.
	if (ISOPK_TOUCH) {
		if (char *pref = SDL_GetPrefPath("LunaMoo", "ISOPackager")) {
			SetAppDataDir(pref);
			SDL_free(pref);
		}
	}

	float scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
	if (scale <= 0)
		scale = 1;
	Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if (ISOPK_TOUCH)
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	// --window WxH starts at that size, e.g. --window 412x900 --layout mobile to try the mobile layout
	// at a phone's size.
	int winW = (int)(1180 * scale), winH = (int)(760 * scale);
	for (int i = 1; i + 1 < argc; ++i) {
		int w, h;
		if (strcmp(argv[i], "--window") == 0 && sscanf(argv[i + 1], "%dx%d", &w, &h) == 2 && w > 100 && h > 100) {
			winW = w;
			winH = h;
		}
	}
	SDL_Window *window = SDL_CreateWindow("ISOPackager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, flags);
	if (!window) {
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
	if (!renderer)
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
	if (!renderer) {
		fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		return 1;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	static std::string iniPath = PathU8(U8Path(AppDataDir()) / "imgui.ini");
	io.IniFilename = iniPath.c_str();
	{
		std::error_code ec;
		fs::create_directories(U8Path(AppDataDir()), ec);
	}

	// A font with wide coverage when the system has one (file names in any language).
	const char *fonts[] = {
#if defined(__APPLE__)
		"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/Library/Fonts/Arial Unicode.ttf",
#elif defined(__ANDROID__)
		"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf",
#else
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/local/share/fonts/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
#endif
		nullptr
	};
	bool fontLoaded = false;
	for (const char **f = fonts; *f && !fontLoaded; ++f) {
		FILE *test = fopen(*f, "rb");
		if (!test)
			continue;
		fclose(test);
		fontLoaded = io.Fonts->AddFontFromFileTTF(*f, 16.0f) != nullptr;
	}
	if (!fontLoaded)
		io.Fonts->AddFontDefault();
	ImGui::GetStyle().FontSizeBase = fontLoaded ? 16.0f : 13.0f;

	ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
	ImGui_ImplSDLRenderer2_Init(renderer);

	gui::PlatformHooks hooks;
	hooks.dpiScale = scale;
	hooks.touch = ISOPK_TOUCH;
	hooks.mobilePlatform = ISOPK_MOBILE_PLATFORM;
#if defined(__ANDROID__)
	{
		float ddpi = 0;
		if (SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0)
			hooks.density = ddpi / 160.0f;
	}
#endif
	hooks.platformName = PlatformName();
	hooks.openUrl = [](const std::string &url) { SDL_OpenURL(url.c_str()); };
#if !ISOPK_TOUCH && !defined(_WIN32)
	const bool zenity = HaveTool("zenity"), kdialog = !zenity && HaveTool("kdialog");
	if (zenity || kdialog) {
		hooks.pickFiles = [zenity](std::vector<std::string> *out) {
			return RunDialog(zenity ? "zenity --file-selection --multiple --separator='\n' --title='Add disc images' "
									  "--file-filter='Disc images | *.iso *.ISO *.gcm *.GCM *.cso *.CSO *.zso *.ZSO *.dax *.DAX *.chd *.CHD *.pbp *.PBP *.wia *.WIA *.rvz *.RVZ' "
									  "--file-filter='All files | *' 2>/dev/null"
									: "kdialog --getopenfilename --multiple --separate-output . 'Disc images (*.iso *.gcm *.cso *.zso *.dax *.chd *.pbp *.wia *.rvz)' "
									  "2>/dev/null",
				out);
		};
		hooks.pickFolder = [zenity](std::string *out) {
			std::vector<std::string> r;
			if (!RunDialog(zenity ? "zenity --file-selection --directory 2>/dev/null" : "kdialog --getexistingdirectory . 2>/dev/null", &r))
				return false;
			*out = r.empty() ? std::string() : r[0];
			return true;
		};
	}
	hooks.reveal = [](const std::string &path) {
		std::error_code ec;
		fs::path p = U8Path(path);
		std::string dir = fs::is_directory(p, ec) ? path : PathU8(p.parent_path());
#if defined(__APPLE__)
		if (fs::is_directory(p, ec))
			Spawn({ "open", path });
		else
			Spawn({ "open", "-R", path });
#else
		Spawn({ "xdg-open", dir });
#endif
	};
#elif defined(__APPLE__) || defined(__ANDROID__)
	hooks.reveal = nullptr;
#endif

	{
		gui::App app(hooks);
		std::vector<std::string> args;
		for (int i = 1; i < argc; ++i)
			args.push_back(argv[i]);
		app.HandleArgs(args);

		bool done = false;
		int idleFrames = 0;
		while (!done) {
			SDL_Event e;
			// Sleep while idle (no input, nothing running), so the app costs no CPU.
			bool got = app.Animating() || idleFrames < 3 ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, 500);
			if (got)
				idleFrames = 0;
			std::vector<std::string> dropped;
			while (got) {
				ImGui_ImplSDL2_ProcessEvent(&e);
				if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE &&
											  e.window.windowID == SDL_GetWindowID(window))) {
					if (app.RequestClose())
						done = true;
				} else if (e.type == SDL_DROPFILE) {
					dropped.push_back(e.drop.file);
					SDL_free(e.drop.file);
				} else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED) {
					app.SetDpiScale(ImGui_ImplSDL2_GetContentScaleForWindow(window));
				}
				got = SDL_PollEvent(&e);
			}
			if (!dropped.empty())
				app.AddPaths(dropped);
			if (app.QuitConfirmed())
				done = true;
			if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
				SDL_Delay(50);
				continue;
			}
			++idleFrames;

			ImGui_ImplSDLRenderer2_NewFrame();
			ImGui_ImplSDL2_NewFrame();
			ImGui::NewFrame();
			app.Frame();
			ImGui::Render();
			SDL_RenderSetScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
			SDL_SetRenderDrawColor(renderer, 20, 22, 26, 255);
			SDL_RenderClear(renderer);
			ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
			// Headless screenshots for docs and CI: ISOPK_SCREENSHOT=file.bmp ISOPK_SCREENSHOT_FRAMES=n
			static const char *shot = getenv("ISOPK_SCREENSHOT");
			static int shotFrames = getenv("ISOPK_SCREENSHOT_FRAMES") ? atoi(getenv("ISOPK_SCREENSHOT_FRAMES")) : 30;
			if (shot && --shotFrames == 0) {
				int w, h;
				SDL_GetRendererOutputSize(renderer, &w, &h);
				SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
				if (s && SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
					SDL_SaveBMP(s, shot);
				SDL_FreeSurface(s);
				done = true;
			}
			SDL_RenderPresent(renderer);
		}
	}

	ImGui_ImplSDLRenderer2_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
