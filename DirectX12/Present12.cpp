// 3DMigoto DX12 - swap chain hooks, per-frame logic, hunting and the on-screen overlay.
#include "State12.h"

#include <algorithm>
#include <cstdio>

namespace mig12 {

// =====================================================================================
// Time
// =====================================================================================
double NowSeconds()
{
	static LARGE_INTEGER freq = []() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
	static LARGE_INTEGER start = []() { LARGE_INTEGER s; QueryPerformanceCounter(&s); return s; }();
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
}

// =====================================================================================
// Overlay: a click-through topmost layered window that follows the game window.
// (Drawing into a DX12 swap chain would need a private pipeline; a GDI window is far
// less intrusive and works for any windowed / borderless game.)
// =====================================================================================
static HWND g_ov = nullptr;
static std::mutex g_ov_mu;
static std::string g_ov_persist, g_ov_msg;
static double g_ov_msg_expiry = 0;
static std::atomic<bool> g_ov_started{ false };
static HFONT g_ov_font = nullptr;

static std::string OverlayText()
{
	std::lock_guard<std::mutex> lk(g_ov_mu);
	std::string t = g_ov_persist;
	if (!g_ov_msg.empty() && NowSeconds() < g_ov_msg_expiry) {
		if (!t.empty())
			t += "\n";
		t += g_ov_msg;
	}
	return t;
}

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m) {
	case WM_TIMER: {
		std::string t = OverlayText();
		if (t.empty() || !g_hwnd || !IsWindow(g_hwnd) || IsIconic(g_hwnd)) {
			ShowWindow(h, SW_HIDE);
			return 0;
		}
		HDC dc = GetDC(h);
		HGDIOBJ old = SelectObject(dc, g_ov_font);
		RECT rc = { 0, 0, 900, 0 };
		DrawTextA(dc, t.c_str(), -1, &rc, DT_CALCRECT | DT_NOPREFIX);
		SelectObject(dc, old);
		ReleaseDC(h, dc);
		POINT p = { 0, 0 };
		ClientToScreen(g_hwnd, &p);
		SetWindowPos(h, HWND_TOPMOST, p.x + 10, p.y + 10, rc.right + 8, rc.bottom + 8, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect(h, nullptr, TRUE);
		return 0;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		RECT rc;
		GetClientRect(h, &rc);
		FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(0, 255, 64));
		HGDIOBJ old = SelectObject(dc, g_ov_font);
		std::string t = OverlayText();
		rc.left += 4;
		rc.top += 4;
		DrawTextA(dc, t.c_str(), -1, &rc, DT_NOPREFIX);
		SelectObject(dc, old);
		EndPaint(h, &ps);
		return 0;
	}
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_ERASEBKGND:
		return 1;
	}
	return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI OverlayThread(LPVOID)
{
	HINSTANCE hi = GetModuleHandleW(nullptr);
	WNDCLASSW wc = {};
	wc.lpfnWndProc = OverlayProc;
	wc.hInstance = hi;
	wc.lpszClassName = L"Migoto12Overlay";
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	RegisterClassW(&wc);
	g_ov_font = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	g_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
		L"Migoto12Overlay", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
	if (!g_ov)
		return 0;
	SetLayeredWindowAttributes(g_ov, RGB(0, 0, 0), 0, LWA_COLORKEY);
	SetTimer(g_ov, 1, 100, nullptr);
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return 0;
}

static void Overlay_Start()
{
	if (g_ov_started.exchange(true))
		return;
	if (g_cfg && !g_cfg->settings.overlay)
		return;
	HANDLE t = CreateThread(nullptr, 0, OverlayThread, nullptr, 0, nullptr);
	if (t)
		CloseHandle(t);
}

void Overlay_SetText(const std::string &text)
{
	std::lock_guard<std::mutex> lk(g_ov_mu);
	g_ov_msg = text;
	g_ov_msg_expiry = NowSeconds() + 4.0;
}

static void Overlay_SetPersistent(const std::string &text)
{
	std::lock_guard<std::mutex> lk(g_ov_mu);
	g_ov_persist = text;
}

void Overlay_Shutdown()
{
	if (g_ov)
		PostMessageW(g_ov, WM_QUIT, 0, 0);
}

// =====================================================================================
// Hunting
// =====================================================================================
enum HuntKind { H_PS, H_VS, H_GS, H_HS, H_DS, H_CS, H_IB, H_VB, H_COUNT };
static const char *const kHuntName[H_COUNT] = { "pixelshader", "vertexshader", "geometryshader", "hullshader",
	"domainshader", "computeshader", "indexbuffer", "vertexbuffer" };
static const char *const kHuntShort[H_COUNT] = { "PS", "VS", "GS", "HS", "DS", "CS", "IB", "VB" };

struct HuntSet {
	std::set<uint64_t> visited;
	std::vector<uint64_t> list;
	int idx = -1;
	bool dirty = false;
	std::atomic<uint64_t> selected{ 0 };
	std::atomic<bool> has_sel{ false };
};
static HuntSet g_hunt[H_COUNT];
static std::mutex g_hunt_mu;

static void Visit(HuntKind k, uint64_t h)
{
	if (g_hunt[k].visited.insert(h).second)
		g_hunt[k].dirty = true;
}

void Hunting_RecordDraw(const PsoInfo *pso, uint32_t ib_hash, bool ib_set, const uint32_t *vb_hash, uint32_t vb_mask)
{
	std::lock_guard<std::mutex> lk(g_hunt_mu);
	if (pso) {
		if (pso->hash[SK_PS]) Visit(H_PS, pso->hash[SK_PS]);
		if (pso->hash[SK_VS]) Visit(H_VS, pso->hash[SK_VS]);
		if (pso->hash[SK_GS]) Visit(H_GS, pso->hash[SK_GS]);
		if (pso->hash[SK_HS]) Visit(H_HS, pso->hash[SK_HS]);
		if (pso->hash[SK_DS]) Visit(H_DS, pso->hash[SK_DS]);
	}
	if (ib_set)
		Visit(H_IB, ib_hash);
	for (int s = 0; s < 16; s++)
		if (vb_mask >> s & 1)
			Visit(H_VB, vb_hash[s]);
}

void Hunting_RecordCompute(const PsoInfo *pso)
{
	if (!pso || !pso->hash[SK_CS])
		return;
	std::lock_guard<std::mutex> lk(g_hunt_mu);
	Visit(H_CS, pso->hash[SK_CS]);
}

static inline bool Sel(HuntKind k, uint64_t h)
{
	return g_hunt[k].has_sel.load(std::memory_order_relaxed) && g_hunt[k].selected.load(std::memory_order_relaxed) == h;
}

bool Hunting_ShouldSkip(const PsoInfo *pso, uint32_t ib_hash, bool ib_set, const uint32_t *vb_hash, uint32_t vb_mask)
{
	if (pso) {
		if (pso->hash[SK_PS] && Sel(H_PS, pso->hash[SK_PS])) return true;
		if (pso->hash[SK_VS] && Sel(H_VS, pso->hash[SK_VS])) return true;
		if (pso->hash[SK_GS] && Sel(H_GS, pso->hash[SK_GS])) return true;
		if (pso->hash[SK_HS] && Sel(H_HS, pso->hash[SK_HS])) return true;
		if (pso->hash[SK_DS] && Sel(H_DS, pso->hash[SK_DS])) return true;
	}
	if (ib_set && Sel(H_IB, ib_hash))
		return true;
	if (g_hunt[H_VB].has_sel.load(std::memory_order_relaxed))
		for (int s = 0; s < 16; s++)
			if ((vb_mask >> s & 1) && Sel(H_VB, vb_hash[s]))
				return true;
	return false;
}

bool Hunting_ShouldSkipCompute(const PsoInfo *pso)
{
	return pso && pso->hash[SK_CS] && Sel(H_CS, pso->hash[SK_CS]);
}

static std::string HuntHashStr(HuntKind k, uint64_t h)
{
	char b[40];
	if (k == H_IB || k == H_VB)
		snprintf(b, sizeof(b), "%08x", (unsigned)h);
	else
		snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
	return b;
}

static void SetClipboardText(const std::string &s)
{
	if (!OpenClipboard(g_hwnd))
		return;
	EmptyClipboard();
	HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
	if (g) {
		memcpy(GlobalLock(g), s.c_str(), s.size() + 1);
		GlobalUnlock(g);
		SetClipboardData(CF_TEXT, g);
	}
	CloseClipboard();
}

static void Hunt_RebuildLists()
{
	for (int k = 0; k < H_COUNT; k++) {
		HuntSet &h = g_hunt[k];
		if (!h.dirty)
			continue;
		h.list.assign(h.visited.begin(), h.visited.end());
		if (h.has_sel) {
			auto it = std::find(h.list.begin(), h.list.end(), h.selected.load());
			h.idx = it == h.list.end() ? -1 : (int)(it - h.list.begin());
		}
		h.dirty = false;
	}
}

static void Hunt_Step(HuntKind k, int dir)
{
	HuntSet &h = g_hunt[k];
	if (h.list.empty())
		return;
	int n = (int)h.list.size();
	if (h.idx < 0)
		h.idx = dir > 0 ? 0 : n - 1;
	else
		h.idx = (h.idx + dir + n) % n;
	h.selected = h.list[h.idx];
	h.has_sel = true;
	LOG("Hunting: %s %d/%d = %s\n", kHuntName[k], h.idx + 1, n, HuntHashStr(k, h.selected).c_str());
}

static void Hunt_Mark(HuntKind k)
{
	HuntSet &h = g_hunt[k];
	if (!h.has_sel)
		return;
	std::string hs = HuntHashStr(k, h.selected);
	LOG(">>>> MARKED %s hash=%s <<<<\n", kHuntName[k], hs.c_str());
	std::string line = "Marked " + std::string(kHuntShort[k]) + ": " + hs;
	Overlay_SetText(line);
	if (g_cfg && g_cfg->settings.marking_clipboard)
		SetClipboardText(hs);
	if (k != H_IB && k != H_VB) {
		std::vector<uint8_t> bytes;
		static const int kind_of[] = { SK_PS, SK_VS, SK_GS, SK_HS, SK_DS, SK_CS };
		if (GetShaderBytes(h.selected, bytes)) {
			CreateDirectoryW(WidenUtf8(g_ini_dir + "\\ShaderCache").c_str(), nullptr);
			WriteFileBytes(g_ini_dir + "\\ShaderCache\\" + hs + "-" + kShaderKindName[kind_of[k]] + ".bin", bytes.data(), bytes.size());
		}
	}
}

static void Hunt_UpdateOverlay()
{
	if (g_hunting.load() != 1) {
		Overlay_SetPersistent(g_hunting.load() == 2 ? "Hunting: suspended" : "");
		return;
	}
	std::string t = "Hunting ON";
	for (int k = 0; k < H_COUNT; k++) {
		HuntSet &h = g_hunt[k];
		if (h.has_sel && h.idx >= 0) {
			char b[128];
			snprintf(b, sizeof(b), "\n%s %d/%d %s", kHuntShort[k], h.idx + 1, (int)h.list.size(), HuntHashStr((HuntKind)k, h.selected).c_str());
			t += b;
		}
	}
	Overlay_SetPersistent(t);
}

// =====================================================================================
// Input
// =====================================================================================
static bool GameHasFocus()
{
	HWND fg = GetForegroundWindow();
	if (!fg)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

static std::atomic<bool> g_reload_requested{ false };

static void PollInput()
{
	if (!g_cfg)
		return;
	static bool focus_last = false;
	bool focus = GameHasFocus();
	KeyDownFn down = [focus](int vk) { return focus && (GetAsyncKeyState(vk) & 0x8000) != 0; };
	if (!focus && !focus_last)
		return;
	focus_last = focus;

	std::shared_lock<std::shared_mutex> lk(g_cfg_lock);
	Config *cfg = g_cfg;

	auto pressed = [&](const std::string &name) {
		auto it = cfg->hunt_keys.find(name);
		return it != cfg->hunt_keys.end() && it->second.Pressed(down);
	};

	if (pressed("toggle_hunting")) {
		int h = g_hunting.load();
		g_hunting = (h == 1) ? 2 : 1;
		LOG("Hunting %s\n", g_hunting.load() == 1 ? "enabled" : "suspended");
	}
	if (pressed("done_hunting")) {
		g_hunting = 0;
		for (int k = 0; k < H_COUNT; k++) {
			g_hunt[k].has_sel = false;
			g_hunt[k].idx = -1;
		}
		LOG("Hunting finished\n");
		Overlay_SetText("Hunting off");
	}
	if (pressed("analyse_frame"))
		FA_Request();
	if (pressed("reload_fixes") || pressed("reload_config"))
		g_reload_requested = true;

	if (g_hunting.load() == 1) {
		std::lock_guard<std::mutex> hl(g_hunt_mu);
		Hunt_RebuildLists();
		for (int k = 0; k < H_COUNT; k++) {
			std::string n = kHuntName[k];
			if (pressed("next_" + n)) Hunt_Step((HuntKind)k, +1);
			if (pressed("previous_" + n)) Hunt_Step((HuntKind)k, -1);
			if (pressed("mark_" + n)) Hunt_Mark((HuntKind)k);
		}
	}

	ExecState st;
	st.env.time = NowSeconds();
	st.env.hunting = g_hunting.load();
	cfg->PollKeys(down, st);
}

// =====================================================================================
// Swap chain hooks
// =====================================================================================
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present1)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSwapChain)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

static HookPoint hp_Present, hp_Present1, hp_ResizeBuffers, hp_CreateSwapChain, hp_CreateSwapChainForHwnd;
static int g_idx_Present = 8, g_idx_Present1 = 22, g_idx_ResizeBuffers = 13;
static thread_local int t_present_depth = 0;

void OnPresentBegin(IDXGISwapChain *sc)
{
	static bool first = true;
	if (first) {
		first = false;
		DXGI_SWAP_CHAIN_DESC d = {};
		if (SUCCEEDED(sc->GetDesc(&d))) {
			g_hwnd = d.OutputWindow;
			g_bb_width = (int)d.BufferDesc.Width;
			g_bb_height = (int)d.BufferDesc.Height;
			LOG("Swap chain: hwnd=%p %dx%d buffers=%u format=%u\n", (void *)g_hwnd, g_bb_width, g_bb_height, d.BufferCount, (unsigned)d.BufferDesc.Format);
		}
		Overlay_Start();
	}

	if (FA_Active())
		FA_Stop();
	PollInput();
	FA_StartIfRequested();

	if (g_cfg) {
		std::shared_lock<std::shared_mutex> lk(g_cfg_lock);
		ExecState st;
		st.env.time = NowSeconds();
		st.env.window_width = st.env.rt_width = g_bb_width;
		st.env.window_height = st.env.rt_height = g_bb_height;
		st.env.hunting = g_hunting.load();
		st.env.analyse_frame = FA_Active();
		st.post = false;
		g_cfg->present.Run(st);
	}
}

void OnPresentEnd(IDXGISwapChain *)
{
	if (g_cfg) {
		std::shared_lock<std::shared_mutex> lk(g_cfg_lock);
		ExecState st;
		st.env.time = NowSeconds();
		st.env.hunting = g_hunting.load();
		st.post = true;
		g_cfg->present.Run(st);
	}
	g_frame_no++;

	if (g_reload_requested.exchange(false)) {
		ReloadConfig();
		Overlay_SetText("Config reloaded");
	}
	{
		std::lock_guard<std::mutex> hl(g_hunt_mu);
		Hunt_RebuildLists();
		Hunt_UpdateOverlay();
	}
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain *self, UINT sync, UINT flags)
{
	bool outer = (t_present_depth++ == 0) && !(flags & DXGI_PRESENT_TEST);
	if (outer)
		OnPresentBegin(self);
	HRESULT hr = ((PFN_Present)hp_Present.Orig(self))(self, sync, flags);
	if (outer)
		OnPresentEnd(self);
	t_present_depth--;
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1 *self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *pp)
{
	bool outer = (t_present_depth++ == 0) && !(flags & DXGI_PRESENT_TEST);
	if (outer)
		OnPresentBegin(self);
	HRESULT hr = ((PFN_Present1)hp_Present1.Orig(self))(self, sync, flags, pp);
	if (outer)
		OnPresentEnd(self);
	t_present_depth--;
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain *self, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
	HRESULT hr = ((PFN_ResizeBuffers)hp_ResizeBuffers.Orig(self))(self, count, w, h, fmt, flags);
	if (SUCCEEDED(hr)) {
		DXGI_SWAP_CHAIN_DESC d = {};
		if (SUCCEEDED(self->GetDesc(&d))) {
			g_bb_width = (int)d.BufferDesc.Width;
			g_bb_height = (int)d.BufferDesc.Height;
		}
	}
	return hr;
}

static void PatchSwapChain(IUnknown *unk)
{
	IDXGISwapChain *sc = nullptr;
	if (FAILED(unk->QueryInterface(__uuidof(IDXGISwapChain), (void **)&sc)) || !sc)
		return;
	void **vt = *(void ***)sc;
	if (!hp_Present.Has(vt)) {
		LOG("Hooking IDXGISwapChain vtable %p\n", (void *)vt);
		hp_Present.Install(vt, g_idx_Present, (void *)&Hook_Present);
		hp_ResizeBuffers.Install(vt, g_idx_ResizeBuffers, (void *)&Hook_ResizeBuffers);
		IDXGISwapChain1 *sc1 = nullptr;
		if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void **)&sc1)) && sc1) {
			hp_Present1.Install(*(void ***)sc1, g_idx_Present1, (void *)&Hook_Present1);
			sc1->Release();
		}
	}
	sc->Release();
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory *self, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **pp)
{
	HRESULT hr = ((PFN_CreateSwapChain)hp_CreateSwapChain.Orig(self))(self, dev, desc, pp);
	if (SUCCEEDED(hr) && pp && *pp)
		PatchSwapChain(*pp);
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2 *self, IUnknown *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out, IDXGISwapChain1 **pp)
{
	HRESULT hr = ((PFN_CreateSwapChainForHwnd)hp_CreateSwapChainForHwnd.Orig(self))(self, dev, hwnd, desc, fs, out, pp);
	if (SUCCEEDED(hr) && pp && *pp) {
		if (hwnd)
			g_hwnd = hwnd;
		PatchSwapChain(*pp);
	}
	return hr;
}

void HookDXGI()
{
	g_idx_Present = VINDEX(IDXGISwapChain, Present, 8);
	g_idx_ResizeBuffers = VINDEX(IDXGISwapChain, ResizeBuffers, 13);
	g_idx_Present1 = VINDEX(IDXGISwapChain1, Present1, 22);
	int idx_csc = VINDEX(IDXGIFactory, CreateSwapChain, 10);
	int idx_cscfh = VINDEX(IDXGIFactory2, CreateSwapChainForHwnd, 15);

	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f)) || !f) {
		LOG("ERROR: unable to create a DXGI factory to hook\n");
		return;
	}
	hp_CreateSwapChain.Install(*(void ***)f, idx_csc, (void *)&Hook_CreateSwapChain);
	IDXGIFactory2 *f2 = nullptr;
	if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory2), (void **)&f2)) && f2) {
		hp_CreateSwapChainForHwnd.Install(*(void ***)f2, idx_cscfh, (void *)&Hook_CreateSwapChainForHwnd);
		f2->Release();
	}
	f->Release();
	LOG("DXGI factory hooks installed\n");
}

} // namespace mig12
