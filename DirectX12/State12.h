// 3DMigoto DX12 - shared state for the Windows / D3D12 glue.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#ifdef _MSC_VER
#include <share.h>
#endif

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Config12.h"
#include "Dump12.h"
#include "Hash12.h"

namespace mig12 {

// ---------------------------------------------------------------------------------
// Logging (d3d12_log.txt next to d3dx.ini)
// ---------------------------------------------------------------------------------
void LogOpen(const std::string &path);
void LogClose();
void LogPrintf(const char *fmt, ...);
#define LOG(...) ::mig12::LogPrintf(__VA_ARGS__)

// ---------------------------------------------------------------------------------
// Vtable hooking
// ---------------------------------------------------------------------------------
struct HookPoint {
	static const int N = 8;
	void **vt[N] = {};
	void *orig[N] = {};
	std::atomic<int> n{ 0 };

	bool Has(void **v) const
	{
		int c = n.load();
		for (int i = 0; i < c; i++)
			if (vt[i] == v)
				return true;
		return false;
	}
	void *Orig(const void *self) const
	{
		void **v = *(void ***)self;
		int c = n.load();
		for (int i = 0; i < c; i++)
			if (vt[i] == v)
				return orig[i];
		return c ? orig[0] : nullptr;
	}
	bool Install(void **vtbl, int index, void *hook);
};

// Decode the vtable slot of a COM method from an MSVC pointer-to-member (vcall thunk).
inline int DecodeVcallThunk(const uint8_t *p)
{
	for (int hop = 0; hop < 4; hop++) {
		if (p[0] == 0xE9) {
			int32_t rel;
			memcpy(&rel, p + 1, 4);
			p = p + 5 + rel;
			continue;
		}
		if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x01) {
			if (p[3] == 0xFF && p[4] == 0x20)
				return 0;
			if (p[3] == 0xFF && p[4] == 0x60)
				return p[5] / 8;
			if (p[3] == 0xFF && p[4] == 0xA0) {
				int32_t d;
				memcpy(&d, p + 5, 4);
				return d / 8;
			}
		}
		return -1;
	}
	return -1;
}

template <typename PMF>
int ResolveIndex(PMF pmf, int fallback, const char *name)
{
#if defined(_MSC_VER)
	if (sizeof(PMF) == sizeof(void *)) {
		uintptr_t u = 0;
		memcpy(&u, &pmf, sizeof(u));
		int idx = DecodeVcallThunk((const uint8_t *)u);
		if (idx >= 0) {
			if (idx != fallback)
				LOG("NOTE: vtable index for %s decoded as %d (table said %d), using decoded\n", name, idx, fallback);
			return idx;
		}
	}
#else
	(void)pmf;
#endif
	return fallback;
}
#define VINDEX(Iface, Method, Fallback) ::mig12::ResolveIndex(&Iface::Method, Fallback, #Iface "::" #Method)

// ---------------------------------------------------------------------------------
// Tracked objects
// ---------------------------------------------------------------------------------
struct ResInfo {
	ID3D12Resource *res = nullptr;
	D3D12_RESOURCE_DIMENSION dim = D3D12_RESOURCE_DIMENSION_UNKNOWN;
	uint64_t va = 0;
	uint64_t size = 0; // buffer width
	bool is_buffer = false;
	bool cpu_visible = false; // upload / custom heap with CPU access
	uint8_t *mapped = nullptr;
	int map_count = 0;
	std::vector<uint8_t> shadow;
	bool has_shadow = false;
	uint32_t version = 1;
	// Texture view tracking
	bool has_srv = false;
	bool has_uav = false;
	bool has_rtv = false;
	bool has_dsv = false;
	struct HEnt {
		uint64_t off, size;
		uint32_t ver, hash;
		bool known;
	};
	std::unordered_map<uint64_t, HEnt> hash_cache;
};

struct ResEx : ResInfo {
	struct Shadow {
		std::unordered_map<uint64_t, std::vector<uint8_t>> chunks;
	};
	Shadow sh;
};

enum ShaderKind { SK_VS = 0, SK_HS, SK_DS, SK_GS, SK_PS, SK_CS, SK_COUNT };
extern const char *const kShaderKindName[SK_COUNT];

struct PsoInfo {
	bool compute = false;
	uint64_t hash[SK_COUNT] = {};
	std::vector<IAElement> layout;
	uint32_t used_slots = 0; // bitmask of input slots referenced by the layout
	int topology_type = 0;   // D3D12_PRIMITIVE_TOPOLOGY_TYPE
};

struct CmdState {
	ID3D12PipelineState *pso = nullptr;
	PsoInfo *pso_info = nullptr;
	D3D12_INDEX_BUFFER_VIEW ib = {};
	bool ib_set = false;
	D3D12_VERTEX_BUFFER_VIEW vb[16] = {};
	bool vb_set[16] = {};
	int topology = 0; // D3D_PRIMITIVE_TOPOLOGY
	// Descriptor heap / root signature tracking for texture overrides
	ID3D12RootSignature *root_sig = nullptr;
	std::vector<ID3D12DescriptorHeap *> desc_heaps;
	D3D12_GPU_DESCRIPTOR_HANDLE desc_tables[32] = {}; // root parameter -> GPU descriptor handle
};

// ---------------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------------
extern std::mutex g_mu; // protects resource / pso / cmd tables
extern std::shared_mutex g_cfg_lock; // shared: draw hooks, exclusive: config reload
extern Config *g_cfg;
extern ID3D12Device *g_device; // primary device
extern std::vector<ID3D12Device *> g_devices; // all hooked devices (for multi-device support)
extern std::string g_ini_dir;
extern std::atomic<uint64_t> g_frame_no;
extern std::atomic<int> g_hunting; // 0 off, 1 on, 2 suspended
// Global flag: true when any feature needing command list hooks is active
// (hunting=1, frame analysis, shader/texture overrides). Checked in draw hooks
// for minimal overhead when nothing needs them.
extern std::atomic<bool> g_hooks_active;
extern HWND g_hwnd;
extern int g_bb_width, g_bb_height;

// Root signature tracking (blob size for descriptor table layout parsing)
extern std::unordered_map<void *, size_t> g_root_signatures;

// Resource tracking (defined in Hooks12.cpp)
extern std::unordered_map<void *, std::unique_ptr<ResEx>> g_res;

// ---------------------------------------------------------------------------------
// Module interfaces
// ---------------------------------------------------------------------------------
// Hooks12.cpp
void InitHooksOnce(const std::string &ini_dir);
void HookDevice(IUnknown *device);
bool FetchBufferBytes(uint64_t va, uint64_t size, std::vector<uint8_t> &out);
uint32_t HashBufferRange(uint64_t va, uint64_t size, bool *known, bool *stable = nullptr);
bool GetShaderBytes(uint64_t hash, std::vector<uint8_t> &out);
bool EnsureGpuResource(CustomResource *r);
void ReloadConfig();
bool WriteFileBytes(const std::string &path, const void *data, size_t n);
std::wstring WidenUtf8(const std::string &s);
std::string Narrow(const std::wstring &w);

// FrameAnalysis12.cpp
struct FADraw {
	bool indexed = false;
	uint32_t a = 0; // vertex count per instance or index count per instance
	uint32_t instance_count = 1, start = 0, start_instance = 0;
	int base_vertex = 0;
	const PsoInfo *pso = nullptr;
	int topology = 0;
	bool ib_set = false;
	D3D12_INDEX_BUFFER_VIEW ib = {};
	uint32_t ib_hash = 0;
	uint32_t vb_hash[16] = {};
	const D3D12_VERTEX_BUFFER_VIEW *vb = nullptr;
	const bool *vb_set = nullptr;
};
void FA_Request();
void FA_StartIfRequested();
void FA_Stop();
bool FA_Active();
void FA_OnDraw(const FADraw &d);

// Present12.cpp
void HookDXGI();
void OnPresentBegin(IDXGISwapChain *sc);
void OnPresentEnd(IDXGISwapChain *sc);
void Hunting_RecordDraw(const PsoInfo *pso, uint32_t ib_hash, bool ib_set, const uint32_t *vb_hash, uint32_t vb_mask);
bool Hunting_ShouldSkip(const PsoInfo *pso, uint32_t ib_hash, bool ib_set, const uint32_t *vb_hash, uint32_t vb_mask);
bool Hunting_ShouldSkipCompute(const PsoInfo *pso);
void Hunting_RecordCompute(const PsoInfo *pso);
void Overlay_SetText(const std::string &text);
void Overlay_Shutdown();
double NowSeconds();

} // namespace mig12
