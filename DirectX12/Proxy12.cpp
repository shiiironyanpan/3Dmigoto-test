// 3DMigoto DX12 - d3d12.dll proxy: loads the real system d3d12.dll, forwards
// every export to it and hooks D3D12CreateDevice so the rest of the tool can
// attach to the device.
#include "State12.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#pragma comment(lib, "dxgi.lib")

using namespace mig12;

// ---------------------------------------------------------------------------------
// Forwarding table (filled in DllMain, consumed by the jump stubs in d3d12_forward.asm)
// ---------------------------------------------------------------------------------
extern "C" {
void *g_ForwardTable[16] = {};
}

static const char *const kFwdNames[16] = {
	"D3D12GetDebugInterface",
	"D3D12SerializeRootSignature",
	"D3D12SerializeVersionedRootSignature",
	"D3D12CreateRootSignatureDeserializer",
	"D3D12CreateVersionedRootSignatureDeserializer",
	"D3D12EnableExperimentalFeatures",
	"D3D12GetInterface",
	"D3D12CoreCreateLayeredDevice",
	"D3D12CoreGetLayeredDeviceSize",
	"D3D12CoreRegisterLayeredDevice",
	"D3D12PIXEventsReplaceBlock",
	"D3D12PIXGetThreadInfo",
	"D3D12PIXNotifyWakeFromFenceSignal",
	"D3D12PIXReportCounter",
	"GetBehaviorValue",
	"SetAppCompatStringPointer",
};

static HMODULE g_self = nullptr;
static HMODULE g_real = nullptr;
typedef HRESULT(WINAPI *PFN_D3D12CreateDevice_t)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
static PFN_D3D12CreateDevice_t g_realCreateDevice = nullptr;

static HRESULT WINAPI MissingExport()
{
	return E_NOTIMPL;
}

// ---------------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------------
namespace mig12 {

std::mutex g_mu;
std::shared_mutex g_cfg_lock;
Config *g_cfg = nullptr;
ID3D12Device *g_device = nullptr;
std::string g_ini_dir;
std::atomic<uint64_t> g_frame_no{ 0 };
std::atomic<int> g_hunting{ 0 };
std::atomic<bool> g_hooks_active{ false };
HWND g_hwnd = nullptr;
int g_bb_width = 0, g_bb_height = 0;

const char *const kShaderKindName[SK_COUNT] = { "vs", "hs", "ds", "gs", "ps", "cs" };

static std::mutex g_hook_mu;
static std::mutex g_log_mu;
static FILE *g_log = nullptr;

std::wstring WidenUtf8(const std::string &s)
{
	if (s.empty())
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

std::string Narrow(const std::wstring &w)
{
	if (w.empty())
		return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

void LogOpen(const std::string &path)
{
	std::lock_guard<std::mutex> lk(g_log_mu);
	if (g_log)
		return;
	g_log = _wfsopen(WidenUtf8(path).c_str(), L"w", _SH_DENYNO);
}

void LogClose()
{
	std::lock_guard<std::mutex> lk(g_log_mu);
	if (g_log) {
		fflush(g_log);
		fclose(g_log);
		g_log = nullptr;
	}
}

void LogPrintf(const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> lk(g_log_mu);
	if (g_log) {
		fputs(buf, g_log);
		fflush(g_log);
	}
}

bool HookPoint::Install(void **vtbl, int index, void *hook)
{
	std::lock_guard<std::mutex> lk(g_hook_mu);
	if (Has(vtbl))
		return true;
	int c = n.load();
	if (c >= N) {
		LOG("ERROR: too many distinct vtables for one hook point\n");
		return false;
	}
	DWORD old = 0;
	if (!VirtualProtect(&vtbl[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
		LOG("ERROR: VirtualProtect failed for vtable %p slot %d\n", (void *)vtbl, index);
		return false;
	}
	vt[c] = vtbl;
	orig[c] = vtbl[index];
	vtbl[index] = hook;
	DWORD tmp;
	VirtualProtect(&vtbl[index], sizeof(void *), old, &tmp);
	n.store(c + 1);
	return true;
}

} // namespace mig12


// ---------------------------------------------------------------------------------
// Crash diagnostics: logs fatal exceptions with a module+offset back trace to d3d12_log.txt
// ---------------------------------------------------------------------------------
static std::string ModuleOf(void *addr, size_t *offset)
{
	HMODULE m = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m) && m) {
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(m, path, MAX_PATH);
		std::wstring w = path;
		size_t s = w.find_last_of(L"\\/");
		if (offset)
			*offset = (size_t)((uint8_t *)addr - (uint8_t *)m);
		return mig12::Narrow(s == std::wstring::npos ? w : w.substr(s + 1));
	}
	if (offset)
		*offset = 0;
	return "?";
}

static LONG WINAPI CrashLogger(EXCEPTION_POINTERS *ep)
{
	DWORD code = ep->ExceptionRecord->ExceptionCode;
	if (code != 0xC0000005 && code != 0xC00000FD && code != 0xC000001D && code != 0xC0000094 && code != 0xC0000096)
		return EXCEPTION_CONTINUE_SEARCH;
	static LONG once = 0;
	if (InterlockedExchange(&once, 1))
		return EXCEPTION_CONTINUE_SEARCH;
	size_t off = 0;
	std::string mod = ModuleOf(ep->ExceptionRecord->ExceptionAddress, &off);
	LOG("\n==== FATAL EXCEPTION 0x%08lx at %p (%s+0x%zx) thread %lu ====\n", code, ep->ExceptionRecord->ExceptionAddress, mod.c_str(), off, GetCurrentThreadId());
	if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2)
		LOG("access violation: %s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void *)ep->ExceptionRecord->ExceptionInformation[1]);
	void *frames[48] = {};
	USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
	for (USHORT i = 0; i < n; i++) {
		size_t o = 0;
		std::string m = ModuleOf(frames[i], &o);
		LOG("  #%02u %s+0x%zx\n", (unsigned)i, m.c_str(), o);
	}
	LOG("==== end of crash report ====\n");
	return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------------
static std::once_flag g_init_once;

static void InitOnce()
{
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(g_self, path, MAX_PATH);
	std::wstring dir = path;
	size_t slash = dir.find_last_of(L"\\/");
	if (slash != std::wstring::npos)
		dir = dir.substr(0, slash);
	g_ini_dir = Narrow(dir);

	LogOpen(g_ini_dir + "\\d3d12_log.txt");
	AddVectoredExceptionHandler(1, CrashLogger);
	LOG("3DMigoto DX12 (modding build) starting\n");
	LOG("Module directory: %s\n", g_ini_dir.c_str());
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	LOG("Host process: %s\n", Narrow(exe).c_str());
	if (!g_real)
		LOG("ERROR: could not load the real system d3d12.dll\n");

	InitHooksOnce(g_ini_dir);
	HookDXGI();
}

extern "C" HRESULT WINAPI My_D3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL minFL, REFIID riid, void **ppDevice)
{
	std::call_once(g_init_once, InitOnce);
	if (!g_realCreateDevice)
		return E_FAIL;
	HRESULT hr = g_realCreateDevice(pAdapter, minFL, riid, ppDevice);
	if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
		LOG("D3D12CreateDevice -> %p (feature level 0x%x)\n", *ppDevice, (unsigned)minFL);
		HookDevice((IUnknown *)*ppDevice);
	}
	return hr;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		g_self = hInst;
		// NOTE: no DisableThreadLibraryCalls here - it must not be used with the static CRT.

		wchar_t sys[MAX_PATH] = {};
		GetSystemDirectoryW(sys, MAX_PATH);
		wcscat_s(sys, MAX_PATH, L"\\d3d12.dll");
		g_real = LoadLibraryW(sys);

		for (int i = 0; i < 16; i++) {
			void *p = g_real ? (void *)GetProcAddress(g_real, kFwdNames[i]) : nullptr;
			g_ForwardTable[i] = p ? p : (void *)&MissingExport;
		}
		if (g_real)
			g_realCreateDevice = (PFN_D3D12CreateDevice_t)GetProcAddress(g_real, "D3D12CreateDevice");
	} else if (reason == DLL_PROCESS_DETACH) {
		LogClose();
	}
	return TRUE;
}
