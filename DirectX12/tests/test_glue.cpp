// Behavioural test of the D3D12 glue logic (Hooks12 + FrameAnalysis12) with fake D3D12 objects.
#include "../Hooks12.cpp"
#include "../FrameAnalysis12.cpp"
#include <cassert>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdarg>
using namespace mig12;

// ---------- POSIX shims for the few Win32 file APIs used --------------------------
static std::string N(const wchar_t* w) { std::string s; while (*w) s += (char)*w++; for (auto& c : s) if (c=='\\') c='/'; return s; }
extern "C" {
int MultiByteToWideChar(UINT, DWORD, const char* s, int n, wchar_t* o, int on) { if (!o) return n; for (int i=0;i<n&&i<on;i++) o[i]=(unsigned char)s[i]; return n; }
HANDLE CreateFileW(LPCWSTR p, DWORD acc, DWORD, void*, DWORD disp, DWORD, HANDLE) {
	int fd = (acc & GENERIC_WRITE) ? open(N(p).c_str(), O_WRONLY|O_CREAT|O_TRUNC, 0644) : open(N(p).c_str(), O_RDONLY);
	return fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)(intptr_t)(fd + 1); }
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* s) { struct stat st; if (fstat((int)(intptr_t)h-1,&st)) return 0; s->QuadPart = st.st_size; return 1; }
BOOL ReadFile(HANDLE h, void* b, DWORD n, DWORD* got, void*) { ssize_t r = read((int)(intptr_t)h-1,b,n); *got = r<0?0:r; return r>=0; }
BOOL WriteFile(HANDLE h, const void* b, DWORD n, DWORD* w, void*) { ssize_t r = write((int)(intptr_t)h-1,b,n); *w = r<0?0:r; return r>=0; }
BOOL CloseHandle(HANDLE h) { close((int)(intptr_t)h-1); return 1; }
BOOL CreateDirectoryW(LPCWSTR p, void*) { return mkdir(N(p).c_str(), 0755) == 0; }
BOOL CreateHardLinkW(LPCWSTR n, LPCWSTR e, void*) { return link(N(e).c_str(), N(n).c_str()) == 0; }
DWORD GetLastError() { return 0; }
void GetLocalTime(SYSTEMTIME* t) { memset(t,0,sizeof(*t)); t->wYear=2026; t->wMonth=10; t->wDay=10; t->wHour=12; }
FILE* _wfsopen(const wchar_t* p, const wchar_t*, int) { return fopen(N(p).c_str(), "w"); }
}
namespace mig12 {
std::wstring WidenUtf8(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::mutex g_mu; std::shared_mutex g_cfg_lock; Config* g_cfg = nullptr; ID3D12Device* g_device = nullptr; std::string g_ini_dir;
std::atomic<uint64_t> g_frame_no{0}; std::atomic<int> g_hunting{0}; HWND g_hwnd = nullptr; int g_bb_width = 1920, g_bb_height = 1080;
const char* const kShaderKindName[SK_COUNT] = {"vs","hs","ds","gs","ps","cs"};
void LogPrintf(const char* f, ...) { va_list ap; va_start(ap,f); vprintf(f,ap); va_end(ap); }
bool HookPoint::Install(void** vtbl, int, void*) { if (Has(vtbl)) return true; vt[n]=vtbl; orig[n]=nullptr; n++; return true; }
bool g_skip_ps = false; uint64_t g_ps_seen = 0;
void Hunting_RecordDraw(const PsoInfo* p, uint32_t, bool, const uint32_t*, uint32_t) { if (p) g_ps_seen = p->hash[SK_PS]; }
bool Hunting_ShouldSkip(const PsoInfo*, uint32_t, bool, const uint32_t*, uint32_t) { return g_skip_ps; }
bool Hunting_ShouldSkipCompute(const PsoInfo*) { return false; } void Hunting_RecordCompute(const PsoInfo*) {}
void Overlay_SetText(const std::string& s) { printf("[overlay] %s\n", s.c_str()); }
double NowSeconds() { return 1.0; }
}

// ---------- fake D3D12 objects ---------------------------------------------------------
struct FakeRes : ID3D12Resource {
	std::vector<uint8_t> mem; uint64_t va; D3D12_RESOURCE_DESC d{};
	HRESULT QueryInterface(REFIID, void**) override { return E_FAIL; } ULONG AddRef() override { return 1; } ULONG Release() override { return 1; }
	HRESULT Map(UINT, const D3D12_RANGE*, void** pp) override { *pp = mem.data(); return 0; }
	void Unmap(UINT, const D3D12_RANGE*) override {}
	D3D12_RESOURCE_DESC GetDesc() override { return d; } D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() override { return va; } };
static uint64_t g_next_va = 0x100000;
struct FakeDev : ID3D12Device {
	HRESULT QueryInterface(REFIID, void**) override { return E_FAIL; } ULONG AddRef() override { return 1; } ULONG Release() override { return 1; }
	HRESULT CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**) override { return E_FAIL; }
	HRESULT CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**) override { return E_FAIL; }
	HRESULT CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**) override { return E_FAIL; }
	HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void** pp) override {
		FakeRes* r = new FakeRes(); r->mem.resize(d->Width); r->va = g_next_va; g_next_va += 0x100000; r->d = *d; *pp = r; return 0; }
	HRESULT CreatePlacedResource(ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) override { return E_FAIL; }
	HRESULT CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) override { return E_FAIL; } };

struct FakeCL { void** vt; };   // first member = vtable pointer, like a COM object
static std::vector<std::string> g_calls;
static void f_DrawIndexed(ID3D12GraphicsCommandList*, UINT ic, UINT inst, UINT si, INT bv, UINT) { g_calls.push_back("DrawIndexed " + std::to_string(ic) + "," + std::to_string(inst) + "," + std::to_string(si) + "," + std::to_string(bv)); }
static void f_Draw(ID3D12GraphicsCommandList*, UINT vc, UINT inst, UINT sv, UINT) { g_calls.push_back("Draw " + std::to_string(vc) + "," + std::to_string(inst) + "," + std::to_string(sv)); }
static void f_IAIB(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW* v) { g_calls.push_back(v ? "IB va=" + std::to_string(v->BufferLocation) + " size=" + std::to_string(v->SizeInBytes) + " fmt=" + std::to_string((int)v->Format) : "IB null"); }
static void f_IAVB(ID3D12GraphicsCommandList*, UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) { g_calls.push_back(v ? "VB" + std::to_string(s) + " va=" + std::to_string(v->BufferLocation) + " size=" + std::to_string(v->SizeInBytes) + " stride=" + std::to_string(v->StrideInBytes) : "VB" + std::to_string(s) + " null"); (void)n; }
static void f_nop() {}

static void hook_up(HookPoint& hp, void** vt, void* fn) { hp.vt[0] = vt; hp.orig[0] = fn; hp.n = 1; }

int main()
{
	g_log_cb = [](const char* s) { fputs(s, stdout); };
	// ---- config with an XXMI-like mod
	mkdir("glue_out", 0755);
	std::vector<uint8_t> pos(64), ib(12);
	for (int i = 0; i < 64; i++) pos[i] = (uint8_t)(i * 7 + 1);
	uint16_t idx[6] = {0, 1, 2, 2, 1, 3}; memcpy(ib.data(), idx, 12);
	uint32_t pos_hash = crc32c(0, pos.data(), pos.size()), ib_hash = crc32c(0, ib.data(), ib.size());
	char ini[2048];
	snprintf(ini, sizeof ini,
		"[Constants]\nglobal $mode = 1\n"
		"[TextureOverrideIB]\nhash = %08x\nhandling = skip\n"
		"[TextureOverrideIBPart]\nhash = %08x\nmatch_first_index = 0\nib = ResourceNewIB\nvb0 = ResourceNewVB\n"
		"if $mode == 1\n\tdrawindexed = 3, 0, 0\nelse\n\tdrawindexed = 6, 0, 0\nendif\n"
		"[TextureOverridePos]\nhash = %08x\noverride_byte_stride = 32\n"
		"[ResourceNewIB]\ntype = Buffer\nformat = DXGI_FORMAT_R32_UINT\ndata = \"IIIIIIIIIIII\"\n"
		"[ResourceNewVB]\ntype = Buffer\nstride = 16\ndata = \"VVVVVVVVVVVVVVVV\"\n",
		ib_hash, ib_hash, pos_hash);
	Config cfg; assert(cfg.LoadText(ini, "glue_out")); g_cfg = &cfg;
	g_cfg->settings.shadow_budget_mb = 64; g_cfg->settings.shadow_max_copy_mb = 64; g_cfg->settings.fa_dedupe = true;
	g_overrides_present = true; g_ini_dir = "glue_out";
	FakeDev dev; g_device = &dev;

	// ---- fake vtable for command lists + hook points
	static void* fvt[64] = {};
	hook_up(hp_DrawIndexedInstanced, fvt, (void*)&f_DrawIndexed); hook_up(hp_DrawInstanced, fvt, (void*)&f_Draw);
	hook_up(hp_IASetIndexBuffer, fvt, (void*)&f_IAIB); hook_up(hp_IASetVertexBuffers, fvt, (void*)&f_IAVB);
	hook_up(hp_CopyBufferRegion, fvt, (void*)&f_nop); hook_up(hp_SetPSO, fvt, (void*)&f_nop); hook_up(hp_IASetTopology, fvt, (void*)&f_nop);
	FakeCL clobj{fvt}; auto* cl = reinterpret_cast<ID3D12GraphicsCommandList*>(&clobj);

	// ---- resources: an upload buffer (mapped) and a default buffer that receives copies
	FakeRes up; up.mem = pos; up.mem.insert(up.mem.end(), ib.begin(), ib.end()); up.va = 0x10000;
	up.d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; up.d.Width = up.mem.size();
	FakeRes vbres; vbres.va = 0x200000; vbres.d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; vbres.d.Width = 1 << 20; vbres.mem.resize(1<<20);
	static void* rvt[16] = {}; up.d = up.d; // vtable ptr for Hook install bookkeeping
	{
		D3D12_RESOURCE_DESC d1 = up.d, d2 = vbres.d;
		TrackResource(&up, d1, true); TrackResource(&vbres, d2, false);
		g_res[&up]->mapped = up.mem.data(); g_res[&up]->map_count = 1;
	}
	(void)rvt;
	// the game copies the vertex data to offset 0x1000 of the big default buffer, indices to 0x3000
	Hook_CopyBufferRegion(cl, &vbres, 0x1000, &up, 0, 64);
	Hook_CopyBufferRegion(cl, &vbres, 0x3000, &up, 64, 12);
	bool known = false;
	assert(HashBufferRange(vbres.va + 0x1000, 64, &known) == pos_hash && known);
	assert(HashBufferRange(vbres.va + 0x3000, 12, &known) == ib_hash && known);
	printf("shadow hashing OK (pos=%08x ib=%08x)\n", pos_hash, ib_hash);

	// ---- PSO with a layout, set on the command list
	PsoInfo pi; pi.hash[SK_VS] = 0x1111; pi.hash[SK_PS] = 0x2222;
	IAElement e; e.semantic = "POSITION"; e.format = 6; e.slot = 0; e.aligned_offset = 0; pi.layout.push_back(e); pi.used_slots = 1;
	void* psoptr = (void*)0xABCD; RegisterPso(psoptr, std::move(pi));
	Hook_SetPSO(cl, (ID3D12PipelineState*)psoptr);
	D3D12_VERTEX_BUFFER_VIEW vv = { vbres.va + 0x1000, 64, 16 };
	D3D12_INDEX_BUFFER_VIEW iv = { vbres.va + 0x3000, 12, (DXGI_FORMAT)57 };
	Hook_IASetVertexBuffers(cl, 0, 1, &vv); Hook_IASetIndexBuffer(cl, &iv); Hook_IASetTopology(cl, 4);

	// ---- 1) overrides: IB skip + part with ib/vb0 replacement + drawindexed
	g_calls.clear();
	Hook_DrawIndexedInstanced(cl, 6, 1, 0, 0, 0);
	for (auto& c : g_calls) printf("  call: %s\n", c.c_str());
	// expected: stride override rebind(VB0 stride 32), IB repl, VB0 repl, drawindexed 3,1,0,0, then restore IB + VB0 ; no original draw
	assert(g_calls.size() >= 6);
	bool saw_draw = false, saw_orig_draw = false;
	for (auto& c : g_calls) { if (c == "DrawIndexed 3,1,0,0") saw_draw = true; if (c == "DrawIndexed 6,1,0,0") saw_orig_draw = true; }
	assert(saw_draw && !saw_orig_draw);
	assert(g_calls.back().find("VB0 va=" + std::to_string(vv.BufferLocation)) == 0);   // restored original VB
	assert(g_calls[g_calls.size()-2].find("IB va=" + std::to_string(iv.BufferLocation)) == 0); // restored original IB
	printf("override draw flow OK\n");

	// ---- 2) a draw that does not match (first_index 3) skips only through TextureOverrideIB handling=skip
	g_calls.clear();
	Hook_DrawIndexedInstanced(cl, 3, 1, 3, 0, 0);
	for (auto& c : g_calls) printf("  call: %s\n", c.c_str());
	for (auto& c : g_calls) assert(c.find("Draw") == std::string::npos);   // IB override skips; no part matched -> nothing drawn

	// ---- 3) hunting skip prevents everything
	g_hunting = 1; g_skip_ps = true; g_calls.clear();
	Hook_DrawIndexedInstanced(cl, 6, 1, 0, 0, 0); assert(g_calls.empty()); g_skip_ps = false; g_hunting = 0;

	// ---- 4) variable change selects the other branch
	cfg.GetVar("mode", false)->v = 2; g_calls.clear();
	Hook_DrawIndexedInstanced(cl, 6, 1, 0, 0, 0);
	bool saw6 = false; for (auto& c : g_calls) if (c == "DrawIndexed 6,1,0,0") saw6 = true; assert(saw6);
	printf("branching OK\n");

	// ---- 4b) pooled 16 MB buffer: sampled hash must be fast, stable and react to writes inside a sampled window
	{
		FakeRes pool; pool.va = 0x40000000; pool.d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; pool.d.Width = 16u << 20; pool.mem.resize(16u << 20);
		D3D12_RESOURCE_DESC pd = pool.d; TrackResource(&pool, pd, false);
		FakeRes src; src.va = 0x50000000; src.d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; src.d.Width = 4096; src.mem.assign(4096, 0x5A);
		D3D12_RESOURCE_DESC sd = src.d; TrackResource(&src, sd, true); g_res[&src]->mapped = src.mem.data(); g_res[&src]->map_count = 1;
		Hook_CopyBufferRegion(cl, &pool, 0, &src, 0, 4096);
		bool k1 = false, st1 = false;
		uint32_t h1 = HashBufferRange(pool.va, 16u << 20, &k1, &st1);
		uint32_t h1b = HashBufferRange(pool.va, 16u << 20, nullptr);
		assert(k1 && st1 && h1 == h1b);
		src.mem.assign(4096, 0x77);
		Hook_CopyBufferRegion(cl, &pool, 0, &src, 0, 4096);       // inside the first sampled window -> cache entry invalidated
		uint32_t h2 = HashBufferRange(pool.va, 16u << 20, nullptr);
		assert(h2 != h1);
		// a mesh-sized sub range of the same pool is hashed exactly (not sampled)
		uint32_t hs = HashBufferRange(pool.va, 4096, nullptr);
		assert(hs == crc32c(0, src.mem.data(), 4096));
		// dynamic upload buffer (mapped, no shadow): hashable but not "stable" (hunting must ignore it)
		FakeRes dyn; dyn.va = 0x60000000; dyn.d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; dyn.d.Width = 256; dyn.mem.assign(256, 1);
		D3D12_RESOURCE_DESC dd = dyn.d; TrackResource(&dyn, dd, true); g_res[&dyn]->mapped = dyn.mem.data(); g_res[&dyn]->map_count = 1;
		bool kd = false, sd2 = true; HashBufferRange(dyn.va, 256, &kd, &sd2); assert(kd && !sd2);
		printf("pooled/sampled/dynamic hashing OK\n");
	}

	// ---- 5) frame analysis writes DX11-compatible files
	FA_Request(); FA_StartIfRequested(); assert(FA_Active());
	cfg.GetVar("mode", false)->v = 1; g_overrides_present = false; g_calls.clear();
	Hook_DrawIndexedInstanced(cl, 6, 1, 0, 0, 0);
	Hook_DrawIndexedInstanced(cl, 6, 1, 0, 0, 0);   // identical second draw -> deduped (hard linked) files
	assert(g_calls.size() == 2);
	FA_Stop();
	printf("frame analysis done in %s\n", g_fa_dir.c_str());
	return 0;
}
