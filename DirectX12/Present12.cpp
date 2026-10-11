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

// Draw text with black outline (matching DX11's DrawOutlinedString)
static void DrawOutlinedText(HDC dc, const char *text, int x, int y, COLORREF color)
{
	// Draw outline (black) at 4 offsets
	SetTextColor(dc, RGB(0, 0, 0));
	TextOutA(dc, x - 1, y, text, (int)strlen(text));
	TextOutA(dc, x + 1, y, text, (int)strlen(text));
	TextOutA(dc, x, y - 1, text, (int)strlen(text));
	TextOutA(dc, x, y + 1, text, (int)strlen(text));
	// Draw main text
	SetTextColor(dc, color);
	TextOutA(dc, x, y, text, (int)strlen(text));
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
		// Make overlay cover the entire game client area for proper text positioning
		RECT client_rc;
		GetClientRect(g_hwnd, &client_rc);
		POINT p = { 0, 0 };
		ClientToScreen(g_hwnd, &p);
		SetWindowPos(h, HWND_TOPMOST, p.x, p.y, client_rc.right, client_rc.bottom, SWP_NOACTIVATE | SWP_SHOWWINDOW);
		InvalidateRect(h, nullptr, TRUE);
		return 0;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		RECT rc;
		GetClientRect(h, &rc);
		// NO black background fill - transparent window with only text (like DX11)
		SetBkMode(dc, TRANSPARENT);
		HGDIOBJ old = SelectObject(dc, g_ov_font);
		std::string t = OverlayText();
		
		// Split text into lines for proper positioning
		std::vector<std::string> lines;
		size_t start = 0;
		while (start < t.size()) {
			size_t end = t.find('\n', start);
			if (end == std::string::npos) end = t.size();
			lines.push_back(t.substr(start, end - start));
			start = end + 1;
		}
		
		if (!lines.empty()) {
			const COLORREF lime_green = RGB(0, 255, 64);
			
			// First line (shader counts) - top center
			SIZE sz;
			GetTextExtentPoint32A(dc, lines[0].c_str(), (int)lines[0].size(), &sz);
			int x = (rc.right - sz.cx) / 2;
			int y = 10;
			DrawOutlinedText(dc, lines[0].c_str(), x, y, lime_green);
			
			// Middle lines (shader info) - below first line, top center
			y += sz.cy + 2;
			for (size_t i = 1; i + 1 < lines.size(); i++) {
				GetTextExtentPoint32A(dc, lines[i].c_str(), (int)lines[i].size(), &sz);
				x = (rc.right - sz.cx) / 2;
				DrawOutlinedText(dc, lines[i].c_str(), x, y, lime_green);
				y += sz.cy + 2;
			}
			
			// Last line (stereo info) - bottom center
			if (lines.size() > 1) {
				GetTextExtentPoint32A(dc, lines.back().c_str(), (int)lines.back().size(), &sz);
				x = (rc.right - sz.cx) / 2;
				y = rc.bottom - sz.cy - 10;
				DrawOutlinedText(dc, lines.back().c_str(), x, y, lime_green);
			}
		}
		
		SelectObject(dc, old);
		EndPaint(h, &ps);
		return 0;
	}
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_ERASEBKGND: {
		// Fill with magenta (color key) so it becomes transparent
		RECT rc;
		GetClientRect(h, &rc);
		HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
		FillRect((HDC)w, &rc, magenta);
		DeleteObject(magenta);
		return 1;
	}
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
	// Use NULL background - we handle transparency via layered window color key
	wc.hbrBackground = nullptr;
	RegisterClassW(&wc);
	g_ov_font = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	g_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
		L"Migoto12Overlay", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
	if (!g_ov)
		return 0;
	// Use magenta as color key (transparent color) - won't conflict with black outline or green text
	SetLayeredWindowAttributes(g_ov, RGB(255, 0, 255), 0, LWA_COLORKEY);
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
	if (g_hunt[k].visited.size() >= 50000) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			LOG("WARNING: more than 50000 distinct hunted items of one kind; ignoring new ones\n");
		}
		return;
	}
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

	// Build shader count string (top center) - matches DX11 CreateShaderCountString
	// Display order: VS, PS, CS, GS, DS, HS, VB, IB (matching DX11)
	// Map to internal enum indices: H_VS=1, H_PS=0, H_CS=5, H_GS=2, H_DS=4, H_HS=3, H_VB=7, H_IB=6
	std::string count_str;
	struct DisplayOrder { int idx; const char* name; bool is_main; };
	DisplayOrder display_order[] = {
		{ H_VS, "VS", true }, { H_PS, "PS", true }, { H_CS, "CS", true },
		{ H_GS, "GS", true }, { H_DS, "DS", true }, { H_HS, "HS", true },
		{ H_VB, "VB", false }, { H_IB, "IB", false }
	};
	for (auto &d : display_order) {
		HuntSet &h = g_hunt[d.idx];
		if (!d.is_main && h.list.empty() && !h.has_sel)
			continue; // Skip VB/IB if not visited and not selected
		if (!count_str.empty())
			count_str += " ";
		char b[64];
		int pos = h.has_sel && h.idx >= 0 ? h.idx + 1 : 0;
		snprintf(b, sizeof(b), "%s:%d/%zu", d.name, pos, h.list.size());
		count_str += b;
	}

	// Build shader info lines (below count string) - matches DX11 DrawShaderInfoLines
	// Display order: VB, IB, VS, HS, DS, GS, PS, CS (matching DX11 DrawShaderInfoLines)
	std::string info_lines;
	DisplayOrder info_order[] = {
		{ H_VB, "VB" }, { H_IB, "IB" }, { H_VS, "VS" }, { H_HS, "HS" },
		{ H_DS, "DS" }, { H_GS, "GS" }, { H_PS, "PS" }, { H_CS, "CS" }
	};
	for (auto &d : info_order) {
		HuntSet &h = g_hunt[d.idx];
		if (h.has_sel && h.idx >= 0) {
			if (!info_lines.empty())
				info_lines += "\n";
			char b[128];
			snprintf(b, sizeof(b), "%s: %s", d.name, HuntHashStr((HuntKind)d.idx, h.selected).c_str());
			info_lines += b;
		}
	}

	// Bottom: stereo info (DX12 doesn't have NVAPI stereo)
	std::string stereo_str = "Stereo disabled";

	// Combine all parts
	std::string t = count_str;
	if (!info_lines.empty()) {
		t += "\n" + info_lines;
	}
	t += "\n" + stereo_str;

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
static bool g_present_enabled = true;

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
		// Like DX11: clear all selections and visited lists, but keep hunting enabled (mode 1)
		// This allows the user to start fresh without disabling hunting entirely
		std::lock_guard<std::mutex> hl(g_hunt_mu);
		for (int k = 0; k < H_COUNT; k++) {
			g_hunt[k].visited.clear();
			g_hunt[k].list.clear();
			g_hunt[k].has_sel = false;
			g_hunt[k].idx = -1;
			g_hunt[k].dirty = true;
		}
		// Ensure hunting stays enabled (mode 1) - matches DX11 DoneHunting behavior
		g_hunting = 1;
		LOG("Hunting selections cleared\n");
		Overlay_SetText("Hunting selections cleared");
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
// Swap chain proxy
//
// The first version patched the shared IDXGISwapChain vtable. Other software in the process
// (Steam overlay, REFramework, ...) hooks the very same Present function / vtable slot, and
// the combination ended in an endless Present -> overlay -> Present recursion (stack overflow).
// So instead of patching anything shared we hand the game a *proxy object* (exactly what the
// DX11 3DMigoto does with HackerSwapChain). The proxy calls the real swap chain through its own,
// untouched vtable, so no other hooker can ever call back into us.
// =====================================================================================
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

class SwapChainProxy final : public IDXGISwapChain4 {
public:
	explicit SwapChainProxy(IDXGISwapChain *real) : r0_(real)
	{
		// the reference we were handed belongs to the proxy
		real->QueryInterface(__uuidof(IDXGISwapChain1), (void **)&r1_);
		real->QueryInterface(__uuidof(IDXGISwapChain2), (void **)&r2_);
		real->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&r3_);
		real->QueryInterface(__uuidof(IDXGISwapChain4), (void **)&r4_);
	}

	// ---- IUnknown
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
	{
		if (!ppv)
			return E_POINTER;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
		    riid == __uuidof(IDXGISwapChain) ||
		    (riid == __uuidof(IDXGISwapChain1) && r1_) || (riid == __uuidof(IDXGISwapChain2) && r2_) ||
		    (riid == __uuidof(IDXGISwapChain3) && r3_) || (riid == __uuidof(IDXGISwapChain4) && r4_)) {
			*ppv = static_cast<IDXGISwapChain4 *>(this);
			AddRef();
			return S_OK;
		}
		return r0_->QueryInterface(riid, ppv); // anything else (vendor interfaces ...) goes straight to the real object
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = --refs_;
		if (r == 0) {
			if (r4_) r4_->Release();
			if (r3_) r3_->Release();
			if (r2_) r2_->Release();
			if (r1_) r1_->Release();
			r0_->Release();
			delete this;
		}
		return r;
	}

	// ---- IDXGIObject
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override { return r0_->SetPrivateData(Name, DataSize, pData); }
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown *pUnknown) override { return r0_->SetPrivateDataInterface(Name, pUnknown); }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT *pDataSize, void *pData) override { return r0_->GetPrivateData(Name, pDataSize, pData); }
	HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override { return r0_->GetParent(riid, ppParent); }

	// ---- IDXGIDeviceSubObject
	HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **ppDevice) override { return r0_->GetDevice(riid, ppDevice); }

	// ---- IDXGISwapChain
	HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override
	{
		bool outer = (t_present_depth++ == 0) && !(Flags & DXGI_PRESENT_TEST) && g_present_enabled;
		if (outer)
			OnPresentBegin(r0_);
		HRESULT hr = r0_->Present(SyncInterval, Flags);
		if (outer)
			OnPresentEnd(r0_);
		t_present_depth--;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) override { return r0_->GetBuffer(Buffer, riid, ppSurface); }
	HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override { return r0_->SetFullscreenState(Fullscreen, pTarget); }
	HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override { return r0_->GetFullscreenState(pFullscreen, ppTarget); }
	HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override { return r0_->GetDesc(pDesc); }
	HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override
	{
		HRESULT hr = r0_->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
		if (SUCCEEDED(hr))
			RefreshSize();
		return hr;
	}
	HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override { return r0_->ResizeTarget(pNewTargetParameters); }
	HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **ppOutput) override { return r0_->GetContainingOutput(ppOutput); }
	HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) override { return r0_->GetFrameStatistics(pStats); }
	HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override { return r0_->GetLastPresentCount(pLastPresentCount); }

	// ---- IDXGISwapChain1
	HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) override { return r1_ ? r1_->GetDesc1(pDesc) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) override { return r1_ ? r1_->GetFullscreenDesc(pDesc) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetHwnd(HWND *pHwnd) override { return r1_ ? r1_->GetHwnd(pHwnd) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void **ppUnk) override { return r1_ ? r1_->GetCoreWindow(refiid, ppUnk) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters) override
	{
		if (!r1_)
			return E_NOINTERFACE;
		bool outer = (t_present_depth++ == 0) && !(PresentFlags & DXGI_PRESENT_TEST) && g_present_enabled;
		if (outer)
			OnPresentBegin(r0_);
		HRESULT hr = r1_->Present1(SyncInterval, PresentFlags, pPresentParameters);
		if (outer)
			OnPresentEnd(r0_);
		t_present_depth--;
		return hr;
	}
	BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return r1_ ? r1_->IsTemporaryMonoSupported() : FALSE; }
	HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) override { return r1_ ? r1_->GetRestrictToOutput(ppRestrictToOutput) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *pColor) override { return r1_ ? r1_->SetBackgroundColor(pColor) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *pColor) override { return r1_ ? r1_->GetBackgroundColor(pColor) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override { return r1_ ? r1_->SetRotation(Rotation) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *pRotation) override { return r1_ ? r1_->GetRotation(pRotation) : E_NOINTERFACE; }

	// ---- IDXGISwapChain2
	HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override { return r2_ ? r2_->SetSourceSize(Width, Height) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *pWidth, UINT *pHeight) override { return r2_ ? r2_->GetSourceSize(pWidth, pHeight) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override { return r2_ ? r2_->SetMaximumFrameLatency(MaxLatency) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *pMaxLatency) override { return r2_ ? r2_->GetMaximumFrameLatency(pMaxLatency) : E_NOINTERFACE; }
	HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return r2_ ? r2_->GetFrameLatencyWaitableObject() : nullptr; }
	HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) override { return r2_ ? r2_->SetMatrixTransform(pMatrix) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) override { return r2_ ? r2_->GetMatrixTransform(pMatrix) : E_NOINTERFACE; }

	// ---- IDXGISwapChain3
	UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return r3_ ? r3_->GetCurrentBackBufferIndex() : 0; }
	HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) override { return r3_ ? r3_->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override { return r3_ ? r3_->SetColorSpace1(ColorSpace) : E_NOINTERFACE; }
	HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT *pCreationNodeMask, IUnknown *const *ppPresentQueue) override
	{
		if (!r3_)
			return E_NOINTERFACE;
		HRESULT hr = r3_->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);
		if (SUCCEEDED(hr))
			RefreshSize();
		return hr;
	}

	// ---- IDXGISwapChain4
	HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) override { return r4_ ? r4_->SetHDRMetaData(Type, Size, pMetaData) : E_NOINTERFACE; }

private:
	void RefreshSize()
	{
		DXGI_SWAP_CHAIN_DESC d = {};
		if (SUCCEEDED(r0_->GetDesc(&d))) {
			g_bb_width = (int)d.BufferDesc.Width;
			g_bb_height = (int)d.BufferDesc.Height;
		}
	}

	std::atomic<ULONG> refs_{ 1 };
	IDXGISwapChain *r0_ = nullptr;
	IDXGISwapChain1 *r1_ = nullptr;
	IDXGISwapChain2 *r2_ = nullptr;
	IDXGISwapChain3 *r3_ = nullptr;
	IDXGISwapChain4 *r4_ = nullptr;
};

// Only swap chains created for a D3D12 command queue are proxied; everything else (D3D11 chains
// made by overlays, etc.) is left completely alone.
static bool IsD3D12Queue(IUnknown *dev)
{
	if (!dev)
		return false;
	ID3D12CommandQueue *q = nullptr;
	if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12CommandQueue), (void **)&q)) && q) {
		q->Release();
		return true;
	}
	return false;
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSwapChain)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateSwapChainForHwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);

static HookPoint hp_CreateSwapChain, hp_CreateSwapChainForHwnd;

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory *self, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **pp)
{
	HRESULT hr = ((PFN_CreateSwapChain)hp_CreateSwapChain.Orig(self))(self, dev, desc, pp);
	if (SUCCEEDED(hr) && pp && *pp && g_present_enabled && IsD3D12Queue(dev)) {
		IDXGISwapChain *real = *pp;
		*pp = new SwapChainProxy(real); // takes over the reference we were given
		LOG("Swap chain created (CreateSwapChain): real=%p proxy=%p\n", (void *)real, (void *)*pp);
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2 *self, IUnknown *dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *out, IDXGISwapChain1 **pp)
{
	HRESULT hr = ((PFN_CreateSwapChainForHwnd)hp_CreateSwapChainForHwnd.Orig(self))(self, dev, hwnd, desc, fs, out, pp);
	if (SUCCEEDED(hr) && pp && *pp && g_present_enabled && IsD3D12Queue(dev)) {
		if (hwnd)
			g_hwnd = hwnd;
		IDXGISwapChain1 *real = *pp;
		SwapChainProxy *proxy = new SwapChainProxy(real);
		*pp = static_cast<IDXGISwapChain1 *>(proxy);
		LOG("Swap chain created (CreateSwapChainForHwnd): real=%p proxy=%p\n", (void *)real, (void *)proxy);
	}
	return hr;
}

void HookDXGI()
{
	g_present_enabled = !g_cfg || g_cfg->settings.hook_present;
	if (!g_present_enabled) {
		LOG("[DX12] hook_present=0: swap chains are not proxied (no hunting keys, no frame analysis)\n");
		return;
	}
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
	LOG("DXGI factory hooks installed (swap chains are proxied, the shared swap chain vtable is NOT patched)\n");
}

} // namespace mig12
