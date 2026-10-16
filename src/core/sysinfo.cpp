#include "core/sysinfo.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "core/util.h"

#if defined(_WIN32)
#include <windows.h>
#if !defined(WINAPI_FAMILY) || WINAPI_FAMILY != WINAPI_FAMILY_APP
#include <shlobj.h>
#endif
#else
#include <sys/resource.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach/mach.h>
#endif
#endif

namespace isopk {

static std::mutex g_dirMutex;
static std::string g_appDataDir;

uint64_t TotalRam() {
#if defined(_WIN32)
	MEMORYSTATUSEX ms{ sizeof(ms) };
	return GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : 0;
#elif defined(__APPLE__)
	uint64_t mem = 0;
	size_t len = sizeof(mem);
	return sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0 ? mem : 0;
#elif defined(__FreeBSD__)
	unsigned long mem = 0;
	size_t len = sizeof(mem);
	return sysctlbyname("hw.physmem", &mem, &len, nullptr, 0) == 0 ? mem : 0;
#else
	long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
	return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0;
#endif
}

uint64_t AvailableRam() {
#if defined(_WIN32)
	MEMORYSTATUSEX ms{ sizeof(ms) };
	return GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys : 0;
#elif defined(__APPLE__)
	vm_statistics64_data_t vm{};
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS)
		return TotalRam() / 2;
	uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
	return (uint64_t)(vm.free_count + vm.inactive_count + vm.purgeable_count) * page;
#elif defined(__linux__)
	// MemAvailable counts the page cache the kernel can give back.
	FILE *f = fopen("/proc/meminfo", "r");
	if (f) {
		char line[256];
		while (fgets(line, sizeof(line), f)) {
			unsigned long long kb;
			if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
				fclose(f);
				return kb * 1024ull;
			}
		}
		fclose(f);
	}
	long pages = sysconf(_SC_AVPHYS_PAGES), page = sysconf(_SC_PAGESIZE);
	return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0;
#elif defined(_SC_AVPHYS_PAGES)
	long pages = sysconf(_SC_AVPHYS_PAGES), page = sysconf(_SC_PAGESIZE);
	return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : TotalRam() / 2;
#else
	return TotalRam() / 2;
#endif
}

unsigned CpuCount() {
	unsigned n = std::thread::hardware_concurrency();
	return n ? n : 4;
}

uint64_t DefaultRamBudget() {
	uint64_t total = TotalRam(), avail = AvailableRam();
	if (!total)
		return 2ull << 30;
	uint64_t budget = avail ? avail / 10 * 6 : total / 2;
	budget = std::min<uint64_t>(budget, total / 4 * 3);
	if (sizeof(void *) == 4)
		budget = std::min<uint64_t>(budget, 1ull << 30);  // 32-bit address space
	return std::max<uint64_t>(budget, 256ull << 20);
}

uint64_t FreeDiskSpace(const std::string &path) {
	std::error_code ec;
	fs::path p = U8Path(path);
	while (!p.empty() && !fs::exists(p, ec))
		p = p.parent_path();
	auto sp = fs::space(p.empty() ? fs::current_path(ec) : p, ec);
	return ec ? UINT64_MAX : sp.available;
}

void SetLowPriority(bool low) {
#if defined(_WIN32)
#if !defined(WINAPI_FAMILY) || WINAPI_FAMILY != WINAPI_FAMILY_APP
	SetPriorityClass(GetCurrentProcess(), low ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
#endif
#else
	// Lowering is always allowed; going back up may need privileges, which is fine to miss.
	setpriority(PRIO_PROCESS, 0, low ? 10 : 0);
#endif
}

void SetAppDataDir(const std::string &dir) {
	std::lock_guard<std::mutex> g(g_dirMutex);
	g_appDataDir = dir;
}

std::string AppDataDir() {
	{
		std::lock_guard<std::mutex> g(g_dirMutex);
		if (!g_appDataDir.empty())
			return g_appDataDir;
	}
	fs::path base;
#if defined(_WIN32) && (!defined(WINAPI_FAMILY) || WINAPI_FAMILY != WINAPI_FAMILY_APP)
	PWSTR p = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
		base = fs::path(p) / "ISOPackager";
		CoTaskMemFree(p);
	}
#elif defined(__APPLE__)
	if (const char *h = getenv("HOME"))
		base = fs::path(h) / "Library" / "Application Support" / "ISOPackager";
#else
	if (const char *x = getenv("XDG_CONFIG_HOME"); x && *x)
		base = fs::path(x) / "isopackager";
	else if (const char *h = getenv("HOME"))
		base = fs::path(h) / ".config" / "isopackager";
#endif
	std::error_code ec;
	if (base.empty())
		base = fs::current_path(ec);
	return PathU8(base);
}

void ConfigureCsoThreads(unsigned threads) {
	if (threads == 0)
		threads = CpuCount();
	threads = std::min(threads, 1024u);
	std::string v = std::to_string(threads);
#ifdef _WIN32
	_putenv_s("UV_THREADPOOL_SIZE", v.c_str());
#else
	setenv("UV_THREADPOOL_SIZE", v.c_str(), 1);
#endif
}

std::string PlatformName() {
	const char *os =
#if defined(__ANDROID__)
		"Android";
#elif defined(_WIN32) && defined(WINAPI_FAMILY) && WINAPI_FAMILY == WINAPI_FAMILY_APP
		"UWP";
#elif defined(_WIN32)
		"Windows";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
		"iOS";
#elif defined(__APPLE__)
		"macOS";
#elif defined(__FreeBSD__)
		"FreeBSD";
#elif defined(__linux__)
		"Linux";
#else
		"Unix";
#endif
	const char *arch =
#if defined(__x86_64__) || defined(_M_X64)
		"x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
		"arm64";
#elif defined(__i386__) || defined(_M_IX86)
		"x86";
#elif defined(__arm__) || defined(_M_ARM)
		"arm";
#else
		"cpu";
#endif
	return std::string(os) + " " + arch;
}

}  // namespace isopk
