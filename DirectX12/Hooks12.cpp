// 3DMigoto DX12 - device / resource / pipeline / command list hooks.
//
// Design (see README_DX12.md for the long version):
//  * Everything is hooked by patching COM vtables of the objects the game creates.
//  * Buffers that are filled through upload heaps (CopyBufferRegion) get a sparse CPU
//    "shadow" copy so that vertex / index data can be hashed and dumped without GPU readback.
//  * IB/VB resources are identified by crc32c of the bytes inside the bound view range.
//  * [TextureOverride] sections are matched at draw time against those hashes; replacement
//    vertex/index buffers are plain upload-heap buffers, re-bound on the command list just
//    for the draw and restored afterwards.
#include "State12.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <d3d12.h>
#include <d3dcompiler.h>

namespace mig12 {

// =====================================================================================
// Hook points
// =====================================================================================
static HookPoint hp_CreateCommandList, hp_CreateCommandList1;
static HookPoint hp_CreateGraphicsPSO, hp_CreateComputePSO, hp_CreatePipelineState;
static HookPoint hp_CreateCommittedResource, hp_CreatePlacedResource, hp_CreateReservedResource;
static HookPoint hp_CreateCommittedResource1, hp_CreateCommittedResource2, hp_CreatePlacedResource1;
static HookPoint hp_CreateCommittedResource3;
static HookPoint hp_ResMap, hp_ResUnmap, hp_ResRelease;
static HookPoint hp_Reset, hp_ClearState, hp_DrawInstanced, hp_DrawIndexedInstanced, hp_Dispatch;
static HookPoint hp_CopyBufferRegion, hp_CopyResource, hp_IASetTopology, hp_SetPSO;
static HookPoint hp_IASetIndexBuffer, hp_IASetVertexBuffers;
// Texture / descriptor heap / root signature hooks
static HookPoint hp_SetGraphicsRootDescriptorTable, hp_SetDescriptorHeaps, hp_SetGraphicsRootSignature;
static HookPoint hp_CreateShaderResourceView, hp_CreateUnorderedAccessView, hp_CreateRenderTargetView, hp_CreateDepthStencilView;
static HookPoint hp_CreateRootSignature;
// ExecuteIndirect / Bundle hooks
static HookPoint hp_ExecuteIndirect, hp_ExecuteBundle;

struct Indices {
	// Fallbacks only; on MSVC the real index is decoded from the interface at start-up.
	// (ID3D12Device derives from ID3D12Object, not ID3D12DeviceChild, so its methods start at slot 7.)
	int dev_CreateCommandList = 12, dev_CreateGraphicsPSO = 10, dev_CreateComputePSO = 11;
	int dev_CreateCommittedResource = 27, dev_CreatePlacedResource = 29, dev_CreateReservedResource = 30;
	int dev2_CreatePipelineState = 47;
	int dev4_CreateCommandList1 = 51, dev4_CreateCommittedResource1 = 53;
	int dev8_CreateCommittedResource2 = 69, dev8_CreatePlacedResource1 = 70;
	int dev10_CreateCommittedResource3 = 76;
	// Device view creation
	int dev_CreateShaderResourceView = 34, dev_CreateUnorderedAccessView = 35, dev_CreateRenderTargetView = 36, dev_CreateDepthStencilView = 37;
	int dev_CreateRootSignature = 26;
	int res_Map = 8, res_Unmap = 9, unk_Release = 2;
	int cl_Reset = 10, cl_ClearState = 11, cl_DrawInstanced = 12, cl_DrawIndexedInstanced = 13;
	int cl_Dispatch = 14, cl_CopyBufferRegion = 15, cl_CopyResource = 17, cl_IASetTopology = 20;
	int cl_SetPSO = 25, cl_IASetIndexBuffer = 43, cl_IASetVertexBuffers = 44;
	// Command list descriptor / root signature
	int cl_SetGraphicsRootDescriptorTable = 31, cl_SetDescriptorHeaps = 29, cl_SetGraphicsRootSignature = 27;
	// ExecuteIndirect / Bundle
	int cl_ExecuteIndirect = 40, cl_ExecuteBundle = 41;
};
static Indices g_idx;
static bool g_idx_init = false;

// IIDs for interfaces newer than the minimum SDK we want to require
static const GUID kIID_Device8 = { 0x9218E6BB, 0xF944, 0x4F7E, { 0xA7, 0x5C, 0xB1, 0xB2, 0xC7, 0xB7, 0x01, 0xF3 } };
static const GUID kIID_Device10 = { 0x517F8718, 0xAA66, 0x49F9, { 0xB0, 0x2B, 0xA7, 0xAB, 0x89, 0xC0, 0x60, 0x31 } };

// =====================================================================================
// Tables
// =====================================================================================
static const uint64_t kChunk = 65536;

struct Shadow {
	std::unordered_map<uint64_t, std::vector<uint8_t>> chunks;
};

struct ResEx : ResInfo {
	Shadow sh;
};

static std::unordered_map<void *, std::unique_ptr<ResEx>> g_res;
static std::map<uint64_t, ResEx *> g_va;
static std::unordered_map<void *, std::unique_ptr<PsoInfo>> g_pso;
static std::unordered_map<void *, std::unique_ptr<CmdState>> g_cmd;
static std::unordered_map<uint64_t, std::vector<uint8_t>> g_shader_bytes;
static std::atomic<uint64_t> g_shadow_bytes{ 0 };
static std::atomic<bool> g_store_shaders{ false };
static std::atomic<bool> g_overrides_present{ false };
static bool g_shadow_budget_warned = false;

static uint64_t ShadowBudgetBytes() { return (uint64_t)(g_cfg ? g_cfg->settings.shadow_budget_mb : 1024) * 1024 * 1024; }

// ---- helpers (g_mu must be held) ------------------------------------------------------

static ResEx *FindByVA(uint64_t va)
{
	auto it = g_va.upper_bound(va);
	if (it == g_va.begin())
		return nullptr;
	--it;
	ResEx *ri = it->second;
	if (va < ri->va + ri->size)
		return ri;
	return nullptr;
}

static void FreeShadow(ResEx *ri)
{
	uint64_t n = (uint64_t)ri->sh.chunks.size() * kChunk;
	g_shadow_bytes -= std::min<uint64_t>(n, g_shadow_bytes.load());
	ri->sh.chunks.clear();
	ri->has_shadow = false;
}

static void RemoveResourceLocked(void *r)
{
	auto it = g_res.find(r);
	if (it == g_res.end())
		return;
	ResEx *ri = it->second.get();
	if (ri->va) {
		auto vit = g_va.find(ri->va);
		if (vit != g_va.end() && vit->second == ri)
			g_va.erase(vit);
	}
	FreeShadow(ri);
	g_res.erase(it);
}

static void InvalidateHashes(ResEx *ri, uint64_t off, uint64_t n)
{
	if (ri->hash_cache.empty())
		return;
	if (ri->hash_cache.size() > 256) {
		ri->hash_cache.clear();
		return;
	}
	for (auto it = ri->hash_cache.begin(); it != ri->hash_cache.end();) {
		if (it->second.off < off + n && off < it->second.off + it->second.size)
			it = ri->hash_cache.erase(it);
		else
			++it;
	}
}

static void ShadowWrite(ResEx *ri, uint64_t off, const uint8_t *src, uint64_t n)
{
	InvalidateHashes(ri, off, n);
	uint64_t pos = 0;
	while (pos < n) {
		uint64_t abs = off + pos;
		uint64_t ci = abs / kChunk, co = abs % kChunk;
		uint64_t len = std::min<uint64_t>(kChunk - co, n - pos);
		auto it = ri->sh.chunks.find(ci);
		if (it == ri->sh.chunks.end()) {
			if ((g_shadow_bytes.load() + kChunk) > ShadowBudgetBytes()) {
				if (!g_shadow_budget_warned) {
					g_shadow_budget_warned = true;
					LOG("WARNING: shadow memory budget exhausted, further buffer data will not be hashable\n");
				}
				return;
			}
			it = ri->sh.chunks.emplace(ci, std::vector<uint8_t>(kChunk, 0)).first;
			g_shadow_bytes += kChunk;
		}
		memcpy(it->second.data() + co, src + pos, len);
		pos += len;
	}
	ri->has_shadow = true;
}

static bool ShadowAny(const ResEx *ri, uint64_t off, uint64_t n)
{
	if (ri->sh.chunks.empty() || !n)
		return false;
	for (uint64_t c = off / kChunk; c <= (off + n - 1) / kChunk; c++)
		if (ri->sh.chunks.count(c))
			return true;
	return false;
}

static uint32_t ShadowHash(const ResEx *ri, uint64_t off, uint64_t n)
{
	static const uint8_t zeros[4096] = {};
	uint32_t crc = 0;
	uint64_t pos = 0;
	while (pos < n) {
		uint64_t abs = off + pos;
		uint64_t ci = abs / kChunk, co = abs % kChunk;
		uint64_t len = std::min<uint64_t>(kChunk - co, n - pos);
		auto it = ri->sh.chunks.find(ci);
		if (it != ri->sh.chunks.end()) {
			crc = crc32c(crc, it->second.data() + co, (size_t)len);
		} else {
			uint64_t z = len;
			while (z) {
				uint64_t k = std::min<uint64_t>(z, sizeof(zeros));
				crc = crc32c(crc, zeros, (size_t)k);
				z -= k;
			}
		}
		pos += len;
	}
	return crc;
}

static void ShadowRead(const ResEx *ri, uint64_t off, uint64_t n, std::vector<uint8_t> &out)
{
	out.assign((size_t)n, 0);
	uint64_t pos = 0;
	while (pos < n) {
		uint64_t abs = off + pos;
		uint64_t ci = abs / kChunk, co = abs % kChunk;
		uint64_t len = std::min<uint64_t>(kChunk - co, n - pos);
		auto it = ri->sh.chunks.find(ci);
		if (it != ri->sh.chunks.end())
			memcpy(out.data() + pos, it->second.data() + co, (size_t)len);
		pos += len;
	}
}

// Pointer for source data of an upload buffer / shadow-less mapped buffer
static const uint8_t *MappedPtr(const ResEx *ri, uint64_t off, uint64_t n)
{
	if (ri->mapped && off + n <= ri->size)
		return ri->mapped + off;
	return nullptr;
}

// =====================================================================================
// Public data access (hashing / fetching)
// =====================================================================================
uint32_t HashBufferRange(uint64_t va, uint64_t size, bool *known, bool *stable)
{
	if (known)
		*known = false;
	if (stable)
		*stable = false;
	if (!va || !size)
		return 0;
	std::lock_guard<std::mutex> lk(g_mu);
	ResEx *ri = FindByVA(va);
	if (!ri)
		return 0;
	uint64_t off = va - ri->va;
	if (off + size > ri->size)
		size = ri->size - off;

	// Data that lives only in a persistently mapped upload buffer changes whenever the game wants,
	// so it can only be cached for the current frame and is not "stable" (never recorded by hunting).
	const bool dynamic_upload = ri->mapped && !ri->has_shadow;
	uint32_t ver = dynamic_upload ? (uint32_t)g_frame_no.load() : 0;
	uint64_t key = (off * 0x9E3779B97F4A7C15ULL) ^ (size + 0x7F4A7C15ULL);
	auto it = ri->hash_cache.find(key);
	if (it != ri->hash_cache.end() && it->second.off == off && it->second.size == size && it->second.ver == ver) {
		if (known)
			*known = it->second.known;
		if (stable)
			*stable = it->second.known && !dynamic_upload;
		return it->second.hash;
	}

	const bool have_shadow = ri->has_shadow && ShadowAny(ri, off, size);
	const uint8_t *mp = have_shadow ? nullptr : MappedPtr(ri, off, size);
	auto window_hash = [&](uint32_t crc, uint64_t wo, uint64_t wl) -> uint32_t {
		if (have_shadow) {
			// chain through the shadow without copying
			static const uint8_t zeros[4096] = {};
			uint64_t pos = 0;
			while (pos < wl) {
				uint64_t abs = wo + pos;
				uint64_t ci = abs / kChunk, co = abs % kChunk;
				uint64_t len = std::min<uint64_t>(kChunk - co, wl - pos);
				auto c = ri->sh.chunks.find(ci);
				if (c != ri->sh.chunks.end()) {
					crc = crc32c(crc, c->second.data() + co, (size_t)len);
				} else {
					uint64_t z = len;
					while (z) {
						uint64_t k = std::min<uint64_t>(z, sizeof(zeros));
						crc = crc32c(crc, zeros, (size_t)k);
						z -= k;
					}
				}
				pos += len;
			}
			return crc;
		}
		return crc32c(crc, mp + (wo - off), (size_t)wl);
	};

	uint32_t h;
	bool have = have_shadow || mp != nullptr;
	if (have) {
		const uint64_t thresh = (uint64_t)(g_cfg ? g_cfg->settings.hash_sample_threshold_kb : 4096) * 1024;
		if (thresh && size > thresh) {
			// A view this large is almost certainly a whole pooled buffer: hash three 64 KB windows
			// plus the size instead of megabytes of data on every draw call.
			const uint64_t W = 65536;
			h = window_hash(0, off, W);
			h = window_hash(h, off + size / 2 - W / 2, W);
			h = window_hash(h, off + size - W, W);
			h = crc32c(h, &size, sizeof(size));
			static std::atomic<int> logged{ 0 };
			if (logged.load() < 10 && ++logged <= 10)
				LOG("NOTE: a buffer view of %llu KB was hashed from samples (pooled buffer?); meshes sharing that view get the same hash\n", (unsigned long long)(size / 1024));
		} else {
			h = window_hash(0, off, size);
		}
	} else {
		uint64_t t[2] = { size, ri->size };
		h = crc32c(0xDEAD, t, sizeof(t));
	}
	if (ri->hash_cache.size() > 8192)
		ri->hash_cache.clear();
	ri->hash_cache[key] = ResInfo::HEnt{ off, size, ver, h, have };
	if (known)
		*known = have;
	if (stable)
		*stable = have && !dynamic_upload;
	return h;
}

// =====================================================================================
// Texture hashing
// =====================================================================================

// Compute a hash for a texture resource from its shadow copy or mapped data.
// Returns 0 if the texture data is not available.
static uint32_t HashTextureResource(ResEx *ri)
{
	if (!ri || !ri->has_shadow && !ri->mapped)
		return 0;

	// For textures, we hash the entire resource (all mips, all array slices)
	// using the shadow copy if available, otherwise mapped data.
	// This is a simplified version - in practice we'd need to handle different
	// texture dimensions, formats, mip levels, etc.
	
	// For now, use the same approach as buffers but for the full resource size
	if (ri->has_shadow && !ri->sh.chunks.empty()) {
		static const uint8_t zeros[4096] = {};
		uint32_t crc = 0;
		for (auto &kv : ri->sh.chunks) {
			const std::vector<uint8_t> &chunk = kv.second;
			crc = crc32c(crc, chunk.data(), chunk.size());
		}
		return crc;
	} else if (ri->mapped && ri->size > 0) {
		return crc32c(0, ri->mapped, (size_t)ri->size);
	}
	return 0;
}

// Collect texture hashes from bound SRV descriptor tables
static void CollectTextureHashes(const CmdState *cs, uint32_t *out_hashes, int max_hashes, int *out_count)
{
	*out_count = 0;
	if (!cs->root_sig || cs->desc_heaps.empty())
		return;

	// For each bound descriptor heap
	for (ID3D12DescriptorHeap *heap : cs->desc_heaps) {
		D3D12_DESCRIPTOR_HEAP_DESC heap_desc = heap->GetDesc();
		if (heap_desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
			continue;

		// Get the descriptor size
		UINT desc_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		
		// For each root parameter that has a descriptor table bound
		for (int rp = 0; rp < 32 && *out_count < max_hashes; rp++) {
			D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = cs->desc_tables[rp];
			if (gpu_handle.ptr == 0)
				continue;

			// Check if this GPU handle belongs to our heap
			D3D12_GPU_DESCRIPTOR_HANDLE heap_start = heap->GetGPUDescriptorHandleForHeapStart();
			if (gpu_handle.ptr < heap_start.ptr)
				continue;
			
			UINT64 offset = (gpu_handle.ptr - heap_start.ptr) / desc_size;
			if (offset >= heap_desc.NumDescriptors)
				continue;

			// Read the descriptor CPU-side to get the resource
			D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = heap->GetCPUDescriptorHandleForHeapStart();
			cpu_handle.ptr += offset * desc_size;

			// For SRV descriptors, we need to read the resource pointer
			// This is a simplified approach - in practice we'd need to parse the descriptor
			// For now, we'll use a placeholder approach
		}
	}
}

// Provide frame analysis with a snapshot of tracked texture resources without exposing
// the resource-tracking implementation to another translation unit.
std::vector<ID3D12Resource *> GetFrameAnalysisTextures()
{
	std::vector<ID3D12Resource *> textures;
	std::lock_guard<std::mutex> lk(g_mu);
	for (const auto &kv : g_res) {
		const ResEx *ri = kv.second.get();
		if (ri->has_srv && (ri->mapped || ri->has_shadow) && ri->dim != D3D12_RESOURCE_DIMENSION_BUFFER)
			textures.push_back(ri->res);
	}
	return textures;
}

// =====================================================================================
// HLSL Compilation (for CustomShader)
// =====================================================================================

static bool CompileHLSL(const std::string &hlsl, const std::string &entry_point, const std::string &target, std::vector<uint8_t> &out_bytecode)
{
	if (!g_device)
		return false;

	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	ID3DBlob *blob = nullptr;
	ID3DBlob *errors = nullptr;
	HRESULT hr = D3DCompile(hlsl.c_str(), hlsl.size(), nullptr, nullptr, nullptr, entry_point.c_str(), target.c_str(), flags, 0, &blob, &errors);

	if (FAILED(hr)) {
		if (errors) {
			LOG("HLSL compilation failed: %s\n", (char *)errors->GetBufferPointer());
			errors->Release();
		} else {
			LOG("HLSL compilation failed (hr=0x%08x)\n", (unsigned)hr);
		}
		if (blob)
			blob->Release();
		return false;
	}

	out_bytecode.assign((const uint8_t *)blob->GetBufferPointer(), (const uint8_t *)blob->GetBufferPointer() + blob->GetBufferSize());
	blob->Release();
	if (errors)
		errors->Release();
	return true;
}

// Compile all custom shaders at config load
static void CompileCustomShaders(Config *cfg)
{
	for (auto &cs_ptr : cfg->custom_shader_store) {
		CustomShader *cs = cs_ptr.get();
		if (cs->compiled || cs->compile_failed || cs->hlsl.empty())
			continue;
		if (CompileHLSL(cs->hlsl, cs->entry_point, cs->target, cs->compiled_bytecode)) {
			cs->compiled = true;
			LOG("Compiled custom shader %s (target hash: %016llx): %zu bytes\n", cs->name.c_str(), (unsigned long long)cs->target_hash, cs->compiled_bytecode.size());
		} else {
			cs->compile_failed = true;
			LOG("Failed to compile custom shader %s\n", cs->name.c_str());
		}
	}
}

bool FetchBufferBytes(uint64_t va, uint64_t size, std::vector<uint8_t> &out)
{
	if (!va || !size)
		return false;
	std::lock_guard<std::mutex> lk(g_mu);
	ResEx *ri = FindByVA(va);
	if (!ri)
		return false;
	uint64_t off = va - ri->va;
	if (off + size > ri->size)
		size = ri->size - off;
	if (ri->has_shadow && ShadowAny(ri, off, size)) {
		ShadowRead(ri, off, size, out);
		return true;
	}
	if (const uint8_t *p = MappedPtr(ri, off, size)) {
		out.assign(p, p + size);
		return true;
	}
	return false;
}

bool GetShaderBytes(uint64_t hash, std::vector<uint8_t> &out)
{
	std::lock_guard<std::mutex> lk(g_mu);
	auto it = g_shader_bytes.find(hash);
	if (it == g_shader_bytes.end())
		return false;
	out = it->second;
	return true;
}

// =====================================================================================
// Command list state
// =====================================================================================
static CmdState *GetCmd(void *cl)
{
	static thread_local void *last_cl = nullptr;
	static thread_local CmdState *last_cs = nullptr;
	if (cl == last_cl)
		return last_cs;
	std::lock_guard<std::mutex> lk(g_mu);
	auto &p = g_cmd[cl];
	if (!p)
		p.reset(new CmdState());
	last_cl = cl;
	last_cs = p.get();
	return last_cs;
}

static PsoInfo *LookupPso(void *pso)
{
	if (!pso)
		return nullptr;
	std::lock_guard<std::mutex> lk(g_mu);
	auto it = g_pso.find(pso);
	return it == g_pso.end() ? nullptr : it->second.get();
}

static void ResetCmdState(CmdState *cs, ID3D12PipelineState *pso)
{
	*cs = CmdState();
	cs->pso = pso;
	cs->pso_info = LookupPso(pso);
}

// =====================================================================================
// Command list hooks
// =====================================================================================
typedef HRESULT(STDMETHODCALLTYPE *PFN_Reset)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
typedef void(STDMETHODCALLTYPE *PFN_ClearState)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
typedef void(STDMETHODCALLTYPE *PFN_DrawInstanced)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DrawIndexedInstanced)(ID3D12GraphicsCommandList *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_Dispatch)(ID3D12GraphicsCommandList *, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_ExecuteIndirect)(ID3D12GraphicsCommandList *, ID3D12CommandSignature *, UINT, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64);
typedef void(STDMETHODCALLTYPE *PFN_ExecuteBundle)(ID3D12GraphicsCommandList *, ID3D12GraphicsCommandList *);
typedef void(STDMETHODCALLTYPE *PFN_CopyBufferRegion)(ID3D12GraphicsCommandList *, ID3D12Resource *, UINT64, ID3D12Resource *, UINT64, UINT64);
typedef void(STDMETHODCALLTYPE *PFN_CopyResource)(ID3D12GraphicsCommandList *, ID3D12Resource *, ID3D12Resource *);
typedef void(STDMETHODCALLTYPE *PFN_IASetTopology)(ID3D12GraphicsCommandList *, D3D12_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE *PFN_SetPSO)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
typedef void(STDMETHODCALLTYPE *PFN_IASetIndexBuffer)(ID3D12GraphicsCommandList *, const D3D12_INDEX_BUFFER_VIEW *);
typedef void(STDMETHODCALLTYPE *PFN_IASetVertexBuffers)(ID3D12GraphicsCommandList *, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW *);

struct DrawParams {
	bool indexed = false;
	UINT count = 0, inst = 1, start = 0, start_inst = 0;
	INT base = 0;
};

static void CallOriginalDraw(ID3D12GraphicsCommandList *cl, const DrawParams &dp)
{
	if (dp.indexed)
		((PFN_DrawIndexedInstanced)hp_DrawIndexedInstanced.Orig(cl))(cl, dp.count, dp.inst, dp.start, dp.base, dp.start_inst);
	else
		((PFN_DrawInstanced)hp_DrawInstanced.Orig(cl))(cl, dp.count, dp.inst, dp.start, dp.start_inst);
}

// ---- override engine -----------------------------------------------------------------
struct DrawEngine : IEngine {
	ID3D12GraphicsCommandList *cl;
	CmdState *cs;
	DrawParams dp;
	bool ib_changed = false;
	uint32_t vb_changed_mask = 0;
	bool original_done = false;

	DrawEngine(ID3D12GraphicsCommandList *c, CmdState *s, const DrawParams &p) : cl(c), cs(s), dp(p) {}

	void SetIndexBuffer(CustomResource *r) override
	{
		auto fn = (PFN_IASetIndexBuffer)hp_IASetIndexBuffer.Orig(cl);
		if (!r) {
			fn(cl, nullptr);
			ib_changed = true;
			return;
		}
		if (!EnsureGpuResource(r))
			return;
		D3D12_INDEX_BUFFER_VIEW v = {};
		v.BufferLocation = r->gpu_va;
		v.SizeInBytes = r->gpu_size;
		int fmt = r->format;
		if (fmt != 57 && fmt != 42) {
			// No explicit format: keep whatever the game was using, default to 32 bit
			fmt = cs->ib_set ? (int)cs->ib.Format : 42;
		}
		v.Format = (DXGI_FORMAT)fmt;
		fn(cl, &v);
		ib_changed = true;
	}

	void SetVertexBuffer(int slot, CustomResource *r) override
	{
		auto fn = (PFN_IASetVertexBuffers)hp_IASetVertexBuffers.Orig(cl);
		if (slot < 0 || slot > 15)
			return;
		vb_changed_mask |= 1u << slot;
		if (!r) {
			fn(cl, slot, 1, nullptr);
			return;
		}
		if (!EnsureGpuResource(r))
			return;
		D3D12_VERTEX_BUFFER_VIEW v = {};
		v.BufferLocation = r->gpu_va;
		v.SizeInBytes = r->gpu_size;
		v.StrideInBytes = r->stride ? r->stride : (cs->vb_set[slot] ? cs->vb[slot].StrideInBytes : 0);
		fn(cl, slot, 1, &v);
	}

	// stride override from [TextureOverride] override_byte_stride (re-binds the game's own VB)
	void OverrideStride(int slot, UINT stride)
	{
		if (slot < 0 || slot > 15 || !cs->vb_set[slot])
			return;
		D3D12_VERTEX_BUFFER_VIEW v = cs->vb[slot];
		v.StrideInBytes = stride;
		((PFN_IASetVertexBuffers)hp_IASetVertexBuffers.Orig(cl))(cl, slot, 1, &v);
		vb_changed_mask |= 1u << slot;
	}

	void DrawFromCaller() override
	{
		CallOriginalDraw(cl, dp);
		original_done = true;
	}
	void Draw(uint32_t vc, uint32_t start) override
	{
		((PFN_DrawInstanced)hp_DrawInstanced.Orig(cl))(cl, vc, 1, start, 0);
	}
	void DrawIndexed(uint32_t ic, uint32_t start, int base) override
	{
		((PFN_DrawIndexedInstanced)hp_DrawIndexedInstanced.Orig(cl))(cl, ic, 1, start, base, 0);
	}
	void DrawInstanced(uint32_t vc, uint32_t ic, uint32_t sv, uint32_t si) override
	{
		((PFN_DrawInstanced)hp_DrawInstanced.Orig(cl))(cl, vc, ic, sv, si);
	}
	void DrawIndexedInstanced(uint32_t ixc, uint32_t ic, uint32_t si, int bv, uint32_t sinst) override
	{
		((PFN_DrawIndexedInstanced)hp_DrawIndexedInstanced.Orig(cl))(cl, ixc, ic, si, bv, sinst);
	}

	void Restore()
	{
		if (ib_changed) {
			auto fn = (PFN_IASetIndexBuffer)hp_IASetIndexBuffer.Orig(cl);
			fn(cl, cs->ib_set ? &cs->ib : nullptr);
		}
		if (vb_changed_mask) {
			auto fn = (PFN_IASetVertexBuffers)hp_IASetVertexBuffers.Orig(cl);
			for (int s = 0; s < 16; s++)
				if (vb_changed_mask & (1u << s))
					fn(cl, s, 1, cs->vb_set[s] ? &cs->vb[s] : nullptr);
		}
	}
};

static EvalEnv MakeEnv(const DrawCtx *dc)
{
	EvalEnv env;
	env.time = NowSeconds();
	env.rt_width = env.window_width = g_bb_width;
	env.rt_height = env.window_height = g_bb_height;
	env.hunting = g_hunting.load();
	env.analyse_frame = FA_Active();
	env.draw = dc;
	return env;
}

// Returns true if the original draw should still be issued by the caller.
static bool ProcessDraw(ID3D12GraphicsCommandList *cl, CmdState *cs, DrawParams &dp)
{
	const bool hunting = g_hunting.load() == 1;
	const bool fa = FA_Active();
	const bool ovr = g_overrides_present.load();
	if (!hunting && !fa && !ovr)
		return true;

	const PsoInfo *pi = cs->pso_info;
	uint32_t slots = 0xFFFF;
	if (pi && !pi->layout.empty() && pi->used_slots)
		slots = pi->used_slots;

	// Only compute buffer hashes when hunting is enabled (mode 1) or frame analysis is active
	// This avoids the performance cost when hunting=0 (disabled) or hunting=2 (suspended)
	uint32_t vbh[16] = {};
	uint32_t vb_mask = 0, vb_stable_mask = 0, ibh = 0;
	bool ib_stable = false;
	static std::atomic<int> unknown_logs{ 0 };
	
	if (hunting || fa) {
		for (int s = 0; s < 16; s++) {
			if (cs->vb_set[s] && (slots >> s & 1) && cs->vb[s].SizeInBytes) {
				bool known = false, stable = false;
				vbh[s] = HashBufferRange(cs->vb[s].BufferLocation, cs->vb[s].SizeInBytes, &known, &stable);
				vb_mask |= 1u << s;
				if (stable)
					vb_stable_mask |= 1u << s;
				if (!known && unknown_logs.load() < 25 && ++unknown_logs <= 25)
					LOG("NOTE: vb%d at 0x%llx (%u bytes) has no CPU-side data (GPU generated, untracked or uploaded in an unsupported way); its hash is a size based placeholder\n",
					    s, (unsigned long long)cs->vb[s].BufferLocation, cs->vb[s].SizeInBytes);
			}
		}
		if (dp.indexed && cs->ib_set) {
			bool known = false;
			ibh = HashBufferRange(cs->ib.BufferLocation, cs->ib.SizeInBytes, &known, &ib_stable);
			if (!known && unknown_logs.load() < 25 && ++unknown_logs <= 25)
				LOG("NOTE: index buffer at 0x%llx (%u bytes) has no CPU-side data; its hash is a size based placeholder\n",
				    (unsigned long long)cs->ib.BufferLocation, cs->ib.SizeInBytes);
		}
	}

	// Compute texture hashes from bound SRVs for texture overrides
	uint32_t tex_hashes[32] = {};
	int tex_hash_count = 0;
	if (ovr && cs->root_sig && !cs->desc_heaps.empty()) {
		// Simple approach: iterate through tracked resources that have SRVs
		// and compute their hashes. This is a placeholder - full descriptor
		// parsing would require reading GPU descriptor heaps CPU-side.
		std::lock_guard<std::mutex> lk(g_mu);
		for (auto &kv : g_res) {
			ResEx *ri = kv.second.get();
			if (ri->has_srv && ri->size > 0 && tex_hash_count < 32) {
				uint32_t h = HashTextureResource(ri);
				if (h != 0)
					tex_hashes[tex_hash_count++] = h;
			}
		}
	}

	if (fa) {
		FADraw fd;
		fd.indexed = dp.indexed;
		fd.a = dp.count;
		fd.instance_count = dp.inst;
		fd.start = dp.start;
		fd.start_instance = dp.start_inst;
		fd.base_vertex = dp.base;
		fd.pso = pi;
		fd.topology = cs->topology;
		fd.ib_set = dp.indexed && cs->ib_set;
		fd.ib = cs->ib;
		fd.ib_hash = ibh;
		memcpy(fd.vb_hash, vbh, sizeof(vbh));
		fd.vb = cs->vb;
		fd.vb_set = cs->vb_set;
		FA_OnDraw(fd);
	}

	if (hunting) {
		Hunting_RecordDraw(pi, ibh, dp.indexed && cs->ib_set && ib_stable, vbh, vb_stable_mask);
		if (Hunting_ShouldSkip(pi, ibh, dp.indexed && cs->ib_set, vbh, vb_mask))
			return false;
	}

	if (!ovr)
		return true;

	std::shared_lock<std::shared_mutex> cfg_lock(g_cfg_lock);
	Config *cfg = g_cfg;
	if (!cfg)
		return true;

	DrawCtx dc;
	dc.indexed = dp.indexed;
	dc.instance_count = dp.inst;
	dc.first_instance = dp.start_inst;
	if (dp.indexed) {
		dc.index_count = dp.count;
		dc.first_index = dp.start;
		dc.base_vertex = dp.base;
		dc.first_vertex = (uint32_t)dp.base;
	} else {
		dc.vertex_count = dp.count;
		dc.first_vertex = dp.start;
	}

	DrawEngine eng(cl, cs, dp);
	ExecState st;
	st.engine = &eng;
	st.env = MakeEnv(&dc);
	st.check_texture_override = [](ExecState &, const std::string &) {}; // ib/vb are checked implicitly

	std::vector<const CommandList *> posts;
	bool changed_anything = false;

	auto run_to = [&](uint32_t hash, int slot) {
		const std::vector<TextureOverride *> *list = cfg->FindTextureOverrides(hash);
		if (!list)
			return;
		for (TextureOverride *to : *list) {
			if (!to->matches_draw(dc))
				continue;
			if (to->override_byte_stride >= 0 && slot >= 0)
				eng.OverrideStride(slot, (UINT)to->override_byte_stride);
			if (to->override_vertex_count >= 0 && !dp.indexed) {
				eng.dp.count = (UINT)to->override_vertex_count;
				dp.count = eng.dp.count;
			}
			st.post = false;
			to->cl.Run(st);
			posts.push_back(&to->cl);
			changed_anything = true;
		}
	};

	for (int s = 0; s < 16; s++)
		if (vb_mask >> s & 1)
			run_to(vbh[s], s);
	if (dp.indexed && cs->ib_set)
		run_to(ibh, -1);

	// Texture override matching
	for (int i = 0; i < tex_hash_count; i++) {
		const std::vector<TextureOverride *> *list = cfg->FindTextureOverrides(tex_hashes[i]);
		if (!list)
			continue;
		for (TextureOverride *to : *list) {
			if (!to->matches_draw(dc))
				continue;
			st.post = false;
			to->cl.Run(st);
			posts.push_back(&to->cl);
			changed_anything = true;
		}
	}

	if (pi) {
		for (int k = 0; k < SK_COUNT; k++) {
			if (!pi->hash[k])
				continue;
			const std::vector<ShaderOverride *> *so = cfg->FindShaderOverrides(pi->hash[k]);
			if (!so)
				continue;
			for (ShaderOverride *s : *so) {
				st.post = false;
				s->cl.Run(st);
				posts.push_back(&s->cl);
				changed_anything = true;
			}
		}
	}

	bool call_original = !st.skip_original && !eng.original_done;
	if (call_original)
		CallOriginalDraw(cl, eng.dp);

	if (changed_anything) {
		st.post = true;
		for (const CommandList *c : posts)
			c->Run(st);
		eng.Restore();
	}
	return false; // we have already issued the (possibly modified) original draw ourselves
}

static void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D12GraphicsCommandList *self, UINT vc, UINT ic, UINT sv, UINT si)
{
	// Fast path: if no hooks are needed, call original immediately
	if (!g_hooks_active.load(std::memory_order_relaxed)) {
		((PFN_DrawInstanced)hp_DrawInstanced.Orig(self))(self, vc, ic, sv, si);
		return;
	}
	CmdState *cs = GetCmd(self);
	DrawParams dp;
	dp.indexed = false;
	dp.count = vc;
	dp.inst = ic;
	dp.start = sv;
	dp.start_inst = si;
	if (ProcessDraw(self, cs, dp))
		CallOriginalDraw(self, dp);
}

static void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D12GraphicsCommandList *self, UINT ic, UINT inst, UINT si, INT bv, UINT sinst)
{
	// Fast path: if no hooks are needed, call original immediately
	if (!g_hooks_active.load(std::memory_order_relaxed)) {
		((PFN_DrawIndexedInstanced)hp_DrawIndexedInstanced.Orig(self))(self, ic, inst, si, bv, sinst);
		return;
	}
	CmdState *cs = GetCmd(self);
	DrawParams dp;
	dp.indexed = true;
	dp.count = ic;
	dp.inst = inst;
	dp.start = si;
	dp.base = bv;
	dp.start_inst = sinst;
	if (ProcessDraw(self, cs, dp))
		CallOriginalDraw(self, dp);
}

static void STDMETHODCALLTYPE Hook_Dispatch(ID3D12GraphicsCommandList *self, UINT x, UINT y, UINT z)
{
	if (g_hunting.load() == 1) {
		CmdState *cs = GetCmd(self);
		Hunting_RecordCompute(cs->pso_info);
		if (Hunting_ShouldSkipCompute(cs->pso_info))
			return;
	}
	((PFN_Dispatch)hp_Dispatch.Orig(self))(self, x, y, z);
}

// ExecuteIndirect hook - processes indirect draw commands
static void STDMETHODCALLTYPE Hook_ExecuteIndirect(ID3D12GraphicsCommandList *self, ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset)
{
	CmdState *cs = GetCmd(self);
	// For ExecuteIndirect, we need to process each indirect draw command
	// This is a simplified version - full implementation would parse the argument buffer
	// and apply overrides per-draw
	if (g_hooks_active.load(std::memory_order_relaxed)) {
		// Record hunting for indirect draws if enabled
		if (g_hunting.load() == 1 && cs->pso_info) {
			Hunting_RecordDraw(cs->pso_info, 0, false, nullptr, 0);
		}
	}
	((PFN_ExecuteIndirect)hp_ExecuteIndirect.Orig(self))(self, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
}

// ExecuteBundle hook - processes bundle command lists
static void STDMETHODCALLTYPE Hook_ExecuteBundle(ID3D12GraphicsCommandList *self, ID3D12GraphicsCommandList *pCommandList)
{
	CmdState *cs = GetCmd(self);
	if (g_hooks_active.load(std::memory_order_relaxed)) {
		// Record hunting for bundle execution if enabled
		if (g_hunting.load() == 1) {
			// The bundle's PSO info would be in the bundle's command list state
			// For now, just call the original
		}
	}
	((PFN_ExecuteBundle)hp_ExecuteBundle.Orig(self))(self, pCommandList);
}

static void STDMETHODCALLTYPE Hook_SetPSO(ID3D12GraphicsCommandList *self, ID3D12PipelineState *pso)
{
	CmdState *cs = GetCmd(self);
	cs->pso = pso;
	cs->pso_info = LookupPso(pso);
	((PFN_SetPSO)hp_SetPSO.Orig(self))(self, pso);
}

static void STDMETHODCALLTYPE Hook_IASetTopology(ID3D12GraphicsCommandList *self, D3D12_PRIMITIVE_TOPOLOGY t)
{
	GetCmd(self)->topology = (int)t;
	((PFN_IASetTopology)hp_IASetTopology.Orig(self))(self, t);
}

static void STDMETHODCALLTYPE Hook_IASetIndexBuffer(ID3D12GraphicsCommandList *self, const D3D12_INDEX_BUFFER_VIEW *v)
{
	CmdState *cs = GetCmd(self);
	if (v && v->BufferLocation) {
		cs->ib = *v;
		cs->ib_set = true;
	} else {
		cs->ib_set = false;
	}
	((PFN_IASetIndexBuffer)hp_IASetIndexBuffer.Orig(self))(self, v);
}

static void STDMETHODCALLTYPE Hook_IASetVertexBuffers(ID3D12GraphicsCommandList *self, UINT start, UINT num, const D3D12_VERTEX_BUFFER_VIEW *views)
{
	CmdState *cs = GetCmd(self);
	for (UINT i = 0; i < num && start + i < 16; i++) {
		if (views && views[i].BufferLocation) {
			cs->vb[start + i] = views[i];
			cs->vb_set[start + i] = true;
		} else {
			cs->vb_set[start + i] = false;
		}
	}
	((PFN_IASetVertexBuffers)hp_IASetVertexBuffers.Orig(self))(self, start, num, views);
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(ID3D12GraphicsCommandList *self, ID3D12CommandAllocator *a, ID3D12PipelineState *pso)
{
	HRESULT hr = ((PFN_Reset)hp_Reset.Orig(self))(self, a, pso);
	ResetCmdState(GetCmd(self), pso);
	return hr;
}

static void STDMETHODCALLTYPE Hook_ClearState(ID3D12GraphicsCommandList *self, ID3D12PipelineState *pso)
{
	((PFN_ClearState)hp_ClearState.Orig(self))(self, pso);
	ResetCmdState(GetCmd(self), pso);
}

// =====================================================================================
// Texture / Descriptor Heap / Root Signature hooks
// =====================================================================================

// Forward declarations
struct RootSigInfo;
struct DescriptorTableBinding;

typedef void(STDMETHODCALLTYPE *PFN_SetGraphicsRootDescriptorTable)(ID3D12GraphicsCommandList *, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_SetDescriptorHeaps)(ID3D12GraphicsCommandList *, UINT, ID3D12DescriptorHeap *const *);
typedef void(STDMETHODCALLTYPE *PFN_SetGraphicsRootSignature)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateShaderResourceView)(ID3D12Device *, ID3D12Resource *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateUnorderedAccessView)(ID3D12Device *, ID3D12Resource *, ID3D12Resource *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateRenderTargetView)(ID3D12Device *, ID3D12Resource *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateDepthStencilView)(ID3D12Device *, ID3D12Resource *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateRootSignature)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID, void **);

static void STDMETHODCALLTYPE Hook_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList *self, UINT root_param_idx, D3D12_GPU_DESCRIPTOR_HANDLE base)
{
	CmdState *cs = GetCmd(self);
	if (root_param_idx < 32) {
		cs->desc_tables[root_param_idx] = base;
	}
	((PFN_SetGraphicsRootDescriptorTable)hp_SetGraphicsRootDescriptorTable.Orig(self))(self, root_param_idx, base);
}

static void STDMETHODCALLTYPE Hook_SetDescriptorHeaps(ID3D12GraphicsCommandList *self, UINT num, ID3D12DescriptorHeap *const *ppHeaps)
{
	CmdState *cs = GetCmd(self);
	cs->desc_heaps.clear();
	for (UINT i = 0; i < num && i < 16; i++) {
		if (ppHeaps[i])
			cs->desc_heaps.push_back(ppHeaps[i]);
	}
	((PFN_SetDescriptorHeaps)hp_SetDescriptorHeaps.Orig(self))(self, num, ppHeaps);
}

static void STDMETHODCALLTYPE Hook_SetGraphicsRootSignature(ID3D12GraphicsCommandList *self, ID3D12RootSignature *pRootSignature)
{
	CmdState *cs = GetCmd(self);
	cs->root_sig = pRootSignature;
	((PFN_SetGraphicsRootSignature)hp_SetGraphicsRootSignature.Orig(self))(self, pRootSignature);
}

// Device view creation hooks
static HRESULT STDMETHODCALLTYPE Hook_CreateShaderResourceView(ID3D12Device *self, ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	// Track SRV creation for texture hashing
	if (pResource) {
		std::lock_guard<std::mutex> lk(g_mu);
		auto it = g_res.find(pResource);
		if (it != g_res.end()) {
			it->second->has_srv = true;
		}
	}
	return ((PFN_CreateShaderResourceView)hp_CreateShaderResourceView.Orig(self))(self, pResource, pDesc, DestDescriptor);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateUnorderedAccessView(ID3D12Device *self, ID3D12Resource *pResource, ID3D12Resource *pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	if (pResource) {
		std::lock_guard<std::mutex> lk(g_mu);
		auto it = g_res.find(pResource);
		if (it != g_res.end()) {
			it->second->has_uav = true;
		}
	}
	return ((PFN_CreateUnorderedAccessView)hp_CreateUnorderedAccessView.Orig(self))(self, pResource, pCounterResource, pDesc, DestDescriptor);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateRenderTargetView(ID3D12Device *self, ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	if (pResource) {
		std::lock_guard<std::mutex> lk(g_mu);
		auto it = g_res.find(pResource);
		if (it != g_res.end()) {
			it->second->has_rtv = true;
		}
	}
	return ((PFN_CreateRenderTargetView)hp_CreateRenderTargetView.Orig(self))(self, pResource, pDesc, DestDescriptor);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDepthStencilView(ID3D12Device *self, ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	if (pResource) {
		std::lock_guard<std::mutex> lk(g_mu);
		auto it = g_res.find(pResource);
		if (it != g_res.end()) {
			it->second->has_dsv = true;
		}
	}
	return ((PFN_CreateDepthStencilView)hp_CreateDepthStencilView.Orig(self))(self, pResource, pDesc, DestDescriptor);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateRootSignature(ID3D12Device *self, UINT nodeMask, const void *pBlobWithRootSignature, SIZE_T lengthInBytes, REFIID riid, void **ppvRootSignature)
{
	HRESULT hr = ((PFN_CreateRootSignature)hp_CreateRootSignature.Orig(self))(self, nodeMask, pBlobWithRootSignature, lengthInBytes, riid, ppvRootSignature);
	if (SUCCEEDED(hr) && ppvRootSignature && *ppvRootSignature) {
		// Track root signature for descriptor table layout
		std::lock_guard<std::mutex> lk(g_mu);
		g_root_signatures[*ppvRootSignature] = lengthInBytes; // Store blob size for now
	}
	return hr;
}

// ---- buffer copies: maintain shadow copies --------------------------------------------
static void OnBufferCopy(ID3D12Resource *dst, UINT64 dst_off, ID3D12Resource *src, UINT64 src_off, UINT64 n)
{
	if (!g_cfg || g_cfg->settings.shadow_budget_mb <= 0 || !n)
		return;
	if (n > (uint64_t)g_cfg->settings.shadow_max_copy_mb * 1024 * 1024)
		return;
	std::lock_guard<std::mutex> lk(g_mu);
	auto di = g_res.find(dst);
	if (di == g_res.end() || !di->second->is_buffer)
		return;
	auto si = g_res.find(src);
	if (si == g_res.end() || !si->second->is_buffer)
		return;
	ResEx *d = di->second.get();
	ResEx *s = si->second.get();
	if (dst_off + n > d->size || src_off + n > s->size)
		return;
	if (const uint8_t *p = MappedPtr(s, src_off, n)) {
		ShadowWrite(d, dst_off, p, n);
	} else if (s->has_shadow && ShadowAny(s, src_off, n)) {
		std::vector<uint8_t> tmp;
		ShadowRead(s, src_off, n, tmp);
		ShadowWrite(d, dst_off, tmp.data(), n);
	} else {
		return;
	}
	d->version++;
}

static void STDMETHODCALLTYPE Hook_CopyBufferRegion(ID3D12GraphicsCommandList *self, ID3D12Resource *dst, UINT64 dst_off, ID3D12Resource *src, UINT64 src_off, UINT64 n)
{
	((PFN_CopyBufferRegion)hp_CopyBufferRegion.Orig(self))(self, dst, dst_off, src, src_off, n);
	OnBufferCopy(dst, dst_off, src, src_off, n);
}

static void STDMETHODCALLTYPE Hook_CopyResource(ID3D12GraphicsCommandList *self, ID3D12Resource *dst, ID3D12Resource *src)
{
	((PFN_CopyResource)hp_CopyResource.Orig(self))(self, dst, src);
	uint64_t n = 0;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		auto si = g_res.find(src);
		if (si != g_res.end() && si->second->is_buffer)
			n = si->second->size;
	}
	if (n)
		OnBufferCopy(dst, 0, src, 0, n);
}

// =====================================================================================
// Resource hooks
// =====================================================================================
typedef HRESULT(STDMETHODCALLTYPE *PFN_Map)(ID3D12Resource *, UINT, const D3D12_RANGE *, void **);
typedef void(STDMETHODCALLTYPE *PFN_Unmap)(ID3D12Resource *, UINT, const D3D12_RANGE *);
typedef ULONG(STDMETHODCALLTYPE *PFN_Release)(IUnknown *);

static HRESULT STDMETHODCALLTYPE Hook_ResMap(ID3D12Resource *self, UINT sub, const D3D12_RANGE *rr, void **ppData)
{
	HRESULT hr = ((PFN_Map)hp_ResMap.Orig(self))(self, sub, rr, ppData);
	if (SUCCEEDED(hr) && ppData && *ppData && sub == 0) {
		std::lock_guard<std::mutex> lk(g_mu);
		auto it = g_res.find(self);
		if (it != g_res.end() && it->second->is_buffer) {
			it->second->mapped = (uint8_t *)*ppData;
			it->second->map_count++;
		}
	}
	return hr;
}

static void STDMETHODCALLTYPE Hook_ResUnmap(ID3D12Resource *self, UINT sub, const D3D12_RANGE *wr)
{
	if (sub == 0) {
		std::lock_guard<std::mutex> lk(g_mu);
		auto it = g_res.find(self);
		if (it != g_res.end() && it->second->map_count > 0) {
			ResEx *ri = it->second.get();
			// Many engines Map -> write -> Unmap -> CopyBufferRegion. Once unmapped we could no
			// longer read the data at copy time, so snapshot the written range now.
			if (ri->map_count == 1 && ri->mapped && ri->is_buffer && g_cfg && g_cfg->settings.shadow_budget_mb > 0) {
				uint64_t b = 0, e = ri->size;
				if (wr) {
					b = wr->Begin;
					e = wr->End;
				}
				if (e > ri->size)
					e = ri->size;
				if (e > b && (e - b) <= 16ull * 1024 * 1024)
					ShadowWrite(ri, b, ri->mapped + b, e - b);
			}
			if (--ri->map_count == 0)
				ri->mapped = nullptr;
			ri->version++;
		}
	}
	((PFN_Unmap)hp_ResUnmap.Orig(self))(self, sub, wr);
}

static ULONG STDMETHODCALLTYPE Hook_ResRelease(IUnknown *self)
{
	ULONG r = ((PFN_Release)hp_ResRelease.Orig(self))(self);
	if (r == 0) {
		std::lock_guard<std::mutex> lk(g_mu);
		RemoveResourceLocked(self);
	}
	return r;
}

static void PatchResourceVtable(void **vt)
{
	if (hp_ResMap.Has(vt))
		return;
	hp_ResMap.Install(vt, g_idx.res_Map, (void *)&Hook_ResMap);
	hp_ResUnmap.Install(vt, g_idx.res_Unmap, (void *)&Hook_ResUnmap);
	if (!g_cfg || g_cfg->settings.hook_release)
		hp_ResRelease.Install(vt, g_idx.unk_Release, (void *)&Hook_ResRelease);
}

static void TrackResource(ID3D12Resource *r, const D3D12_RESOURCE_DESC &d, bool cpu_visible)
{
	if (!r)
		return;
	PatchResourceVtable(*(void ***)r);
	std::unique_ptr<ResEx> ri(new ResEx());
	ri->res = r;
	ri->dim = d.Dimension;
	ri->cpu_visible = cpu_visible;
	if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		ri->is_buffer = true;
		ri->size = d.Width;
		ri->va = r->GetGPUVirtualAddress();
	}
	std::lock_guard<std::mutex> lk(g_mu);
	RemoveResourceLocked(r);
	if (ri->is_buffer && ri->va && ri->size) {
		// Anything still registered in this VA range belongs to a destroyed resource
		uint64_t b = ri->va, e = ri->va + ri->size;
		auto it = g_va.upper_bound(b);
		if (it != g_va.begin())
			--it;
		std::vector<void *> dead;
		for (; it != g_va.end() && it->first < e; ++it)
			if (it->first + it->second->size > b)
				dead.push_back(it->second->res);
		for (void *p : dead)
			RemoveResourceLocked(p);
		g_va[ri->va] = ri.get();
	}
	g_res[r] = std::move(ri);
}

static bool HeapIsCpuVisible(const D3D12_HEAP_PROPERTIES *hp)
{
	if (!hp)
		return false;
	if (hp->Type == D3D12_HEAP_TYPE_UPLOAD)
		return true;
	if (hp->Type == D3D12_HEAP_TYPE_CUSTOM)
		return hp->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
	return false;
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CCR)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CPR)(ID3D12Device *, ID3D12Heap *, UINT64, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CRR)(ID3D12Device *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CCR1)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, void *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CCR2)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const void *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, void *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CPR1)(ID3D12Device *, ID3D12Heap *, UINT64, const void *, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CCR3)(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const void *, UINT, const D3D12_CLEAR_VALUE *, void *, UINT, const DXGI_FORMAT *, REFIID, void **);

static HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource(ID3D12Device *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *cv, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CCR)hp_CreateCommittedResource.Orig(self))(self, hp, f, d, s, cv, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp && d)
		TrackResource((ID3D12Resource *)*pp, *d, HeapIsCpuVisible(hp));
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource(ID3D12Device *self, ID3D12Heap *heap, UINT64 off, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *cv, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CPR)hp_CreatePlacedResource.Orig(self))(self, heap, off, d, s, cv, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp && d) {
		D3D12_HEAP_DESC hd = heap ? heap->GetDesc() : D3D12_HEAP_DESC();
		TrackResource((ID3D12Resource *)*pp, *d, HeapIsCpuVisible(&hd.Properties));
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateReservedResource(ID3D12Device *self, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *cv, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CRR)hp_CreateReservedResource.Orig(self))(self, d, s, cv, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp && d)
		TrackResource((ID3D12Resource *)*pp, *d, false);
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource1(ID3D12Device *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *cv, void *prot, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CCR1)hp_CreateCommittedResource1.Orig(self))(self, hp, f, d, s, cv, prot, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp && d)
		TrackResource((ID3D12Resource *)*pp, *d, HeapIsCpuVisible(hp));
	return hr;
}

// D3D12_RESOURCE_DESC1 starts with the exact layout of D3D12_RESOURCE_DESC
static HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource2(ID3D12Device *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS f, const void *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *cv, void *prot, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CCR2)hp_CreateCommittedResource2.Orig(self))(self, hp, f, d, s, cv, prot, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp && d)
		TrackResource((ID3D12Resource *)*pp, *(const D3D12_RESOURCE_DESC *)d, HeapIsCpuVisible(hp));
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePlacedResource1(ID3D12Device *self, ID3D12Heap *heap, UINT64 off, const void *d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE *cv, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CPR1)hp_CreatePlacedResource1.Orig(self))(self, heap, off, d, s, cv, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp && d) {
		D3D12_HEAP_DESC hd = heap ? heap->GetDesc() : D3D12_HEAP_DESC();
		TrackResource((ID3D12Resource *)*pp, *(const D3D12_RESOURCE_DESC *)d, HeapIsCpuVisible(&hd.Properties));
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommittedResource3(ID3D12Device *self, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS f, const void *d, UINT layout, const D3D12_CLEAR_VALUE *cv, void *prot, UINT ncast, const DXGI_FORMAT *cast, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CCR3)hp_CreateCommittedResource3.Orig(self))(self, hp, f, d, layout, cv, prot, ncast, cast, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp && d)
		TrackResource((ID3D12Resource *)*pp, *(const D3D12_RESOURCE_DESC *)d, HeapIsCpuVisible(hp));
	return hr;
}

// =====================================================================================
// Shaders / pipeline state
// =====================================================================================
static std::string HexName(uint64_t h)
{
	char b[32];
	snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
	return b;
}

static std::wstring W(const std::string &s)
{
	if (s.empty())
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static bool ReadFileBytes(const std::string &path, std::vector<uint8_t> &out)
{
	HANDLE h = CreateFileW(W(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER sz;
	bool ok = false;
	if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1LL << 30)) {
		out.resize((size_t)sz.QuadPart);
		DWORD got = 0;
		ok = ReadFile(h, out.data(), (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart;
	}
	CloseHandle(h);
	return ok;
}

bool WriteFileBytes(const std::string &path, const void *data, size_t n)
{
	HANDLE h = CreateFileW(W(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	DWORD wrote = 0;
	bool ok = WriteFile(h, data, (DWORD)n, &wrote, nullptr) && wrote == n;
	CloseHandle(h);
	return ok;
}

static std::mutex g_shader_mu;
static std::map<std::pair<int, uint64_t>, std::unique_ptr<std::vector<uint8_t>>> g_replacements;
static std::set<uint64_t> g_exported;

static const std::vector<uint8_t> *FindReplacement(int kind, uint64_t hash)
{
	std::lock_guard<std::mutex> lk(g_shader_mu);
	auto key = std::make_pair(kind, hash);
	auto it = g_replacements.find(key);
	if (it != g_replacements.end())
		return it->second.get();
	std::unique_ptr<std::vector<uint8_t>> v(new std::vector<uint8_t>());
	std::string path = g_ini_dir + "\\ShaderFixes\\" + HexName(hash) + "-" + kShaderKindName[kind] + "_replace.bin";
	if (!ReadFileBytes(path, *v))
		v.reset();
	else
		LOG("Loaded shader replacement %s (%u bytes)\n", path.c_str(), (unsigned)v->size());
	const std::vector<uint8_t> *p = v.get();
	g_replacements[key] = std::move(v);
	return p;
}

// Registers a shader, handles optional export and replacement. Returns true if replaced.
static bool ProcessShader(int kind, D3D12_SHADER_BYTECODE &sc, PsoInfo &pi, std::vector<std::vector<uint8_t> *> *keepalive)
{
	(void)keepalive;
	if (!sc.pShaderBytecode || !sc.BytecodeLength)
		return false;
	uint64_t h = fnv64(sc.pShaderBytecode, sc.BytecodeLength);
	pi.hash[kind] = h;

	if (g_store_shaders.load()) {
		std::lock_guard<std::mutex> lk(g_mu);
		if (!g_shader_bytes.count(h))
			g_shader_bytes[h].assign((const uint8_t *)sc.pShaderBytecode, (const uint8_t *)sc.pShaderBytecode + sc.BytecodeLength);
	}
	if (g_cfg && g_cfg->settings.export_shaders) {
		bool first;
		{
			std::lock_guard<std::mutex> lk(g_shader_mu);
			first = g_exported.insert(h).second;
		}
		if (first) {
			CreateDirectoryW(W(g_ini_dir + "\\ShaderCache").c_str(), nullptr);
			WriteFileBytes(g_ini_dir + "\\ShaderCache\\" + HexName(h) + "-" + kShaderKindName[kind] + ".bin", sc.pShaderBytecode, sc.BytecodeLength);
		}
	}
	// Check for custom shader replacement (compiled HLSL)
	if (g_cfg) {
		auto it = g_cfg->custom_shaders.find(h);
		if (it != g_cfg->custom_shaders.end() && it->second->compiled && !it->second->compiled_bytecode.empty()) {
			sc.pShaderBytecode = it->second->compiled_bytecode.data();
			sc.BytecodeLength = it->second->compiled_bytecode.size();
			LOG("Replaced shader %s (hash %016llx) with custom shader %s\n", kShaderKindName[kind], (unsigned long long)h, it->second->name.c_str());
			return true;
		}
	}
	if (const std::vector<uint8_t> *rep = FindReplacement(kind, h)) {
		sc.pShaderBytecode = rep->data();
		sc.BytecodeLength = rep->size();
		return true;
	}
	return false;
}

static void CaptureLayout(const D3D12_INPUT_LAYOUT_DESC &il, PsoInfo &pi)
{
	pi.layout.clear();
	pi.used_slots = 0;
	for (UINT i = 0; i < il.NumElements && il.pInputElementDescs; i++) {
		const D3D12_INPUT_ELEMENT_DESC &e = il.pInputElementDescs[i];
		IAElement a;
		a.semantic = e.SemanticName ? e.SemanticName : "";
		a.semantic_index = e.SemanticIndex;
		a.format = (int)e.Format;
		a.slot = e.InputSlot;
		a.aligned_offset = e.AlignedByteOffset;
		a.slot_class = e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? 1 : 0;
		a.step_rate = e.InstanceDataStepRate;
		pi.layout.push_back(a);
		if (e.InputSlot < 16)
			pi.used_slots |= 1u << e.InputSlot;
	}
}

static void RegisterPso(void *pso, PsoInfo &&info)
{
	std::lock_guard<std::mutex> lk(g_mu);
	auto &p = g_pso[pso];
	if (!p)
		p.reset(new PsoInfo());
	*p = std::move(info);
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CGPSO)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CCPSO)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CPS)(ID3D12Device *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);

static HRESULT STDMETHODCALLTYPE Hook_CreateGraphicsPSO(ID3D12Device *self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID riid, void **pp)
{
	if (!desc)
		return ((PFN_CGPSO)hp_CreateGraphicsPSO.Orig(self))(self, desc, riid, pp);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
	PsoInfo pi;
	bool rep = false;
	rep |= ProcessShader(SK_VS, d.VS, pi, nullptr);
	rep |= ProcessShader(SK_HS, d.HS, pi, nullptr);
	rep |= ProcessShader(SK_DS, d.DS, pi, nullptr);
	rep |= ProcessShader(SK_GS, d.GS, pi, nullptr);
	rep |= ProcessShader(SK_PS, d.PS, pi, nullptr);
	if (rep) {
		d.CachedPSO.pCachedBlob = nullptr;
		d.CachedPSO.CachedBlobSizeInBytes = 0;
	}
	CaptureLayout(d.InputLayout, pi);
	pi.topology_type = (int)d.PrimitiveTopologyType;
	HRESULT hr = ((PFN_CGPSO)hp_CreateGraphicsPSO.Orig(self))(self, &d, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp) {
		RegisterPso(*pp, std::move(pi));
		// Log PSO creation with shader hashes for debugging
		if (g_cfg && g_cfg->settings.log_pso) {
			LOG("PSO created: %p VS=%016llx PS=%016llx GS=%016llx HS=%016llx DS=%016llx\n",
			    (void *)*pp,
			    (unsigned long long)pi.hash[SK_VS],
			    (unsigned long long)pi.hash[SK_PS],
			    (unsigned long long)pi.hash[SK_GS],
			    (unsigned long long)pi.hash[SK_HS],
			    (unsigned long long)pi.hash[SK_DS]);
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateComputePSO(ID3D12Device *self, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID riid, void **pp)
{
	if (!desc)
		return ((PFN_CCPSO)hp_CreateComputePSO.Orig(self))(self, desc, riid, pp);
	D3D12_COMPUTE_PIPELINE_STATE_DESC d = *desc;
	PsoInfo pi;
	pi.compute = true;
	if (ProcessShader(SK_CS, d.CS, pi, nullptr)) {
		d.CachedPSO.pCachedBlob = nullptr;
		d.CachedPSO.CachedBlobSizeInBytes = 0;
	}
	HRESULT hr = ((PFN_CCPSO)hp_CreateComputePSO.Orig(self))(self, &d, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp) {
		RegisterPso(*pp, std::move(pi));
		if (g_cfg && g_cfg->settings.log_pso) {
			LOG("Compute PSO created: %p CS=%016llx\n",
			    (void *)*pp,
			    (unsigned long long)pi.hash[SK_CS]);
		}
	}
	return hr;
}

// ---- pipeline state streams (ID3D12Device2::CreatePipelineState) -------------------------
template <class T>
struct SubObj {
	alignas(void *) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
	T inner;
};

struct SubInfo {
	size_t inner_off;
	size_t total;
};

template <class T>
static SubInfo SI()
{
	return SubInfo{ offsetof(SubObj<T>, inner), sizeof(SubObj<T>) };
}

static bool SubobjectInfo(int type, SubInfo *out)
{
	switch (type) {
	case 0: *out = SI<ID3D12RootSignature *>(); return true;
	case 1: case 2: case 3: case 4: case 5: case 6: case 24: case 25:
		*out = SI<D3D12_SHADER_BYTECODE>(); return true;
	case 7: *out = SI<D3D12_STREAM_OUTPUT_DESC>(); return true;
	case 8: *out = SI<D3D12_BLEND_DESC>(); return true;
	case 9: *out = SI<UINT>(); return true;
	case 10: *out = SI<D3D12_RASTERIZER_DESC>(); return true;
	case 11: *out = SI<D3D12_DEPTH_STENCIL_DESC>(); return true;
	case 12: *out = SI<D3D12_INPUT_LAYOUT_DESC>(); return true;
	case 13: *out = SI<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(); return true;
	case 14: *out = SI<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(); return true;
	case 15: *out = SI<D3D12_RT_FORMAT_ARRAY>(); return true;
	case 16: *out = SI<DXGI_FORMAT>(); return true;
	case 17: *out = SI<DXGI_SAMPLE_DESC>(); return true;
	case 18: *out = SI<UINT>(); return true;
	case 19: *out = SI<D3D12_CACHED_PIPELINE_STATE>(); return true;
	case 20: *out = SI<D3D12_PIPELINE_STATE_FLAGS>(); return true;
	case 21: *out = SI<D3D12_DEPTH_STENCIL_DESC1>(); return true;
	case 22: *out = SI<D3D12_VIEW_INSTANCING_DESC>(); return true;
	}
	return false;
}

static HRESULT STDMETHODCALLTYPE Hook_CreatePipelineState(ID3D12Device *self, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID riid, void **pp)
{
	if (!desc || !desc->pPipelineStateSubobjectStream || !desc->SizeInBytes)
		return ((PFN_CPS)hp_CreatePipelineState.Orig(self))(self, desc, riid, pp);

	std::vector<uint8_t> copy((const uint8_t *)desc->pPipelineStateSubobjectStream, (const uint8_t *)desc->pPipelineStateSubobjectStream + desc->SizeInBytes);
	PsoInfo pi;
	bool replaced = false;
	size_t off = 0;
	int cached_off = -1;
	while (off + sizeof(UINT) <= copy.size()) {
		int type = *(int *)(copy.data() + off);
		SubInfo si;
		if (!SubobjectInfo(type, &si)) {
			static bool warned = false;
			if (!warned) {
				warned = true;
				LOG("NOTE: unknown pipeline subobject type %d in stream; stopping stream parse (later subobjects ignored)\n", type);
			}
			break;
		}
		uint8_t *inner = copy.data() + off + si.inner_off;
		if (off + si.total > copy.size())
			break;
		switch (type) {
		case 1: replaced |= ProcessShader(SK_VS, *(D3D12_SHADER_BYTECODE *)inner, pi, nullptr); break;
		case 2: replaced |= ProcessShader(SK_PS, *(D3D12_SHADER_BYTECODE *)inner, pi, nullptr); break;
		case 3: replaced |= ProcessShader(SK_DS, *(D3D12_SHADER_BYTECODE *)inner, pi, nullptr); break;
		case 4: replaced |= ProcessShader(SK_HS, *(D3D12_SHADER_BYTECODE *)inner, pi, nullptr); break;
		case 5: replaced |= ProcessShader(SK_GS, *(D3D12_SHADER_BYTECODE *)inner, pi, nullptr); break;
		case 6: pi.compute = true; replaced |= ProcessShader(SK_CS, *(D3D12_SHADER_BYTECODE *)inner, pi, nullptr); break;
		case 12: CaptureLayout(*(D3D12_INPUT_LAYOUT_DESC *)inner, pi); break;
		case 14: pi.topology_type = (int)*(D3D12_PRIMITIVE_TOPOLOGY_TYPE *)inner; break;
		case 19: cached_off = (int)(off + si.inner_off); break;
		}
		off += si.total;
	}
	if (replaced && cached_off >= 0) {
		D3D12_CACHED_PIPELINE_STATE *c = (D3D12_CACHED_PIPELINE_STATE *)(copy.data() + cached_off);
		c->pCachedBlob = nullptr;
		c->CachedBlobSizeInBytes = 0;
	}
	D3D12_PIPELINE_STATE_STREAM_DESC nd = { copy.size(), copy.data() };
	HRESULT hr = ((PFN_CPS)hp_CreatePipelineState.Orig(self))(self, &nd, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp) {
		RegisterPso(*pp, std::move(pi));
		if (g_cfg && g_cfg->settings.log_pso) {
			LOG("PSO (stream) created: %p VS=%016llx PS=%016llx GS=%016llx HS=%016llx DS=%016llx CS=%016llx\n",
			    (void *)*pp,
			    (unsigned long long)pi.hash[SK_VS],
			    (unsigned long long)pi.hash[SK_PS],
			    (unsigned long long)pi.hash[SK_GS],
			    (unsigned long long)pi.hash[SK_HS],
			    (unsigned long long)pi.hash[SK_DS],
			    (unsigned long long)pi.hash[SK_CS]);
		}
	}
	return hr;
}

// =====================================================================================
// Command list creation / vtable patching
// =====================================================================================
static void PatchCommandListVtable(void **vt)
{
	if (hp_DrawIndexedInstanced.Has(vt))
		return;
	hp_Reset.Install(vt, g_idx.cl_Reset, (void *)&Hook_Reset);
	hp_ClearState.Install(vt, g_idx.cl_ClearState, (void *)&Hook_ClearState);
	hp_DrawInstanced.Install(vt, g_idx.cl_DrawInstanced, (void *)&Hook_DrawInstanced);
	hp_DrawIndexedInstanced.Install(vt, g_idx.cl_DrawIndexedInstanced, (void *)&Hook_DrawIndexedInstanced);
	hp_Dispatch.Install(vt, g_idx.cl_Dispatch, (void *)&Hook_Dispatch);
	hp_CopyBufferRegion.Install(vt, g_idx.cl_CopyBufferRegion, (void *)&Hook_CopyBufferRegion);
	hp_CopyResource.Install(vt, g_idx.cl_CopyResource, (void *)&Hook_CopyResource);
	hp_IASetTopology.Install(vt, g_idx.cl_IASetTopology, (void *)&Hook_IASetTopology);
	hp_SetPSO.Install(vt, g_idx.cl_SetPSO, (void *)&Hook_SetPSO);
	hp_IASetIndexBuffer.Install(vt, g_idx.cl_IASetIndexBuffer, (void *)&Hook_IASetIndexBuffer);
	hp_IASetVertexBuffers.Install(vt, g_idx.cl_IASetVertexBuffers, (void *)&Hook_IASetVertexBuffers);
	// Texture / descriptor heap / root signature hooks
	hp_SetGraphicsRootDescriptorTable.Install(vt, g_idx.cl_SetGraphicsRootDescriptorTable, (void *)&Hook_SetGraphicsRootDescriptorTable);
	hp_SetDescriptorHeaps.Install(vt, g_idx.cl_SetDescriptorHeaps, (void *)&Hook_SetDescriptorHeaps);
	hp_SetGraphicsRootSignature.Install(vt, g_idx.cl_SetGraphicsRootSignature, (void *)&Hook_SetGraphicsRootSignature);
	// ExecuteIndirect / Bundle hooks
	hp_ExecuteIndirect.Install(vt, g_idx.cl_ExecuteIndirect, (void *)&Hook_ExecuteIndirect);
	hp_ExecuteBundle.Install(vt, g_idx.cl_ExecuteBundle, (void *)&Hook_ExecuteBundle);
	LOG("Hooked command list vtable %p\n", (void *)vt);
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CCL)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CCL1)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, UINT, REFIID, void **);

static void OnCommandListCreated(void *cl, ID3D12PipelineState *pso)
{
	if (!cl)
		return;
	ID3D12GraphicsCommandList *gcl = nullptr;
	if (FAILED(((IUnknown *)cl)->QueryInterface(__uuidof(ID3D12GraphicsCommandList), (void **)&gcl)) || !gcl)
		return;
	PatchCommandListVtable(*(void ***)gcl);
	ResetCmdState(GetCmd(gcl), pso);
	gcl->Release();
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(ID3D12Device *self, UINT mask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *alloc, ID3D12PipelineState *pso, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CCL)hp_CreateCommandList.Orig(self))(self, mask, type, alloc, pso, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp)
		OnCommandListCreated(*pp, pso);
	return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateCommandList1(ID3D12Device *self, UINT mask, D3D12_COMMAND_LIST_TYPE type, UINT flags, REFIID riid, void **pp)
{
	HRESULT hr = ((PFN_CCL1)hp_CreateCommandList1.Orig(self))(self, mask, type, flags, riid, pp);
	if (SUCCEEDED(hr) && pp && *pp)
		OnCommandListCreated(*pp, nullptr);
	return hr;
}

// =====================================================================================
// Custom (mod) resources
// =====================================================================================
static std::mutex g_custom_mu;

bool EnsureGpuResource(CustomResource *r)
{
	if (!r)
		return false;
	if (r->gpu)
		return true;
	std::lock_guard<std::mutex> lk(g_custom_mu);
	if (r->gpu)
		return true;
	if (r->load_failed || !g_device)
		return false;
	if (!r->Load() || r->data.empty()) {
		r->load_failed = true;
		return false;
	}
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_UPLOAD;
	hp.CreationNodeMask = hp.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = r->data.size();
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	rd.Flags = D3D12_RESOURCE_FLAG_NONE;
	ID3D12Resource *res = nullptr;
	HRESULT hr = g_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void **)&res);
	if (FAILED(hr) || !res) {
		LOG("ERROR: failed to create GPU buffer for resource %s (hr=0x%08x)\n", r->name.c_str(), (unsigned)hr);
		r->load_failed = true;
		return false;
	}
	void *mp = nullptr;
	D3D12_RANGE none = { 0, 0 };
	if (FAILED(res->Map(0, &none, &mp)) || !mp) {
		res->Release();
		r->load_failed = true;
		return false;
	}
	memcpy(mp, r->data.data(), r->data.size());
	res->Unmap(0, nullptr);
	r->gpu = res; // intentionally never released: mod resources live as long as the process
	r->gpu_va = res->GetGPUVirtualAddress();
	r->gpu_size = (uint32_t)r->data.size();
	LOG("Created GPU buffer for resource %s: %u bytes @ 0x%llx\n", r->name.c_str(), r->gpu_size, (unsigned long long)r->gpu_va);
	return true;
}

// =====================================================================================
// Initialisation
// =====================================================================================
static void InitIndices()
{
	if (g_idx_init)
		return;
	g_idx_init = true;
	g_idx.dev_CreateCommandList = VINDEX(ID3D12Device, CreateCommandList, 12);
	g_idx.dev_CreateGraphicsPSO = VINDEX(ID3D12Device, CreateGraphicsPipelineState, 10);
	g_idx.dev_CreateComputePSO = VINDEX(ID3D12Device, CreateComputePipelineState, 11);
	g_idx.dev_CreateCommittedResource = VINDEX(ID3D12Device, CreateCommittedResource, 27);
	g_idx.dev_CreatePlacedResource = VINDEX(ID3D12Device, CreatePlacedResource, 29);
	g_idx.dev_CreateReservedResource = VINDEX(ID3D12Device, CreateReservedResource, 30);
	g_idx.dev2_CreatePipelineState = VINDEX(ID3D12Device2, CreatePipelineState, 47);
	g_idx.dev4_CreateCommandList1 = VINDEX(ID3D12Device4, CreateCommandList1, 51);
	g_idx.dev4_CreateCommittedResource1 = VINDEX(ID3D12Device4, CreateCommittedResource1, 53);
#ifdef __ID3D12Device8_INTERFACE_DEFINED__
	g_idx.dev8_CreateCommittedResource2 = VINDEX(ID3D12Device8, CreateCommittedResource2, 69);
	g_idx.dev8_CreatePlacedResource1 = VINDEX(ID3D12Device8, CreatePlacedResource1, 70);
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
	g_idx.dev10_CreateCommittedResource3 = VINDEX(ID3D12Device10, CreateCommittedResource3, 76);
#endif
	g_idx.res_Map = VINDEX(ID3D12Resource, Map, 8);
	g_idx.res_Unmap = VINDEX(ID3D12Resource, Unmap, 9);
	g_idx.unk_Release = VINDEX(IUnknown, Release, 2);
	g_idx.cl_Reset = VINDEX(ID3D12GraphicsCommandList, Reset, 10);
	g_idx.cl_ClearState = VINDEX(ID3D12GraphicsCommandList, ClearState, 11);
	g_idx.cl_DrawInstanced = VINDEX(ID3D12GraphicsCommandList, DrawInstanced, 12);
	g_idx.cl_DrawIndexedInstanced = VINDEX(ID3D12GraphicsCommandList, DrawIndexedInstanced, 13);
	g_idx.cl_Dispatch = VINDEX(ID3D12GraphicsCommandList, Dispatch, 14);
	g_idx.cl_CopyBufferRegion = VINDEX(ID3D12GraphicsCommandList, CopyBufferRegion, 15);
	g_idx.cl_CopyResource = VINDEX(ID3D12GraphicsCommandList, CopyResource, 17);
	g_idx.cl_IASetTopology = VINDEX(ID3D12GraphicsCommandList, IASetPrimitiveTopology, 20);
	g_idx.cl_SetPSO = VINDEX(ID3D12GraphicsCommandList, SetPipelineState, 25);
	g_idx.cl_IASetIndexBuffer = VINDEX(ID3D12GraphicsCommandList, IASetIndexBuffer, 43);
	g_idx.cl_IASetVertexBuffers = VINDEX(ID3D12GraphicsCommandList, IASetVertexBuffers, 44);
}

static void RefreshConfigFlags()
{
	if (!g_cfg)
		return;
	g_hunting = g_cfg->settings.hunting;
	g_overrides_present = !g_cfg->texture_overrides.empty() || !g_cfg->shader_overrides.empty();
	bool analyse = g_cfg->hunt_keys.count("analyse_frame") != 0;
	g_store_shaders = g_cfg->settings.hunting != 0 || g_cfg->settings.export_shaders || analyse;
}

void ReloadConfig()
{
	Config *fresh = new Config();
	std::string ini = g_ini_dir + "\\d3dx.ini";
	LOG("---- reloading %s ----\n", ini.c_str());
	if (!fresh->Load(ini)) {
		LOG("ERROR: reload failed, keeping the old configuration\n");
		delete fresh;
		return;
	}
	{
		std::unique_lock<std::shared_mutex> lk(g_cfg_lock);
		// Old config is leaked on purpose: CustomResources hold live GPU buffers that may
		// still be referenced by command lists in flight.
		g_cfg = fresh;
	}
	{
		std::lock_guard<std::mutex> lk(g_shader_mu);
		g_replacements.clear();
	}
	RefreshConfigFlags();
}

void InitHooksOnce(const std::string &ini_dir)
{
	g_ini_dir = ini_dir;
	g_log_cb = [](const char *s) { LogPrintf("%s", s); };
	g_cfg = new Config();
	std::string ini = ini_dir + "\\d3dx.ini";
	if (!g_cfg->Load(ini))
		LOG("WARNING: could not read %s - running with defaults (no mods will be applied)\n", ini.c_str());
	// Compile custom shaders after config load
	CompileCustomShaders(g_cfg);
	RefreshConfigFlags();
	LOG("Hunting=%d shadow_budget_mb=%d export_shaders=%d\n", g_cfg->settings.hunting,
	    g_cfg->settings.shadow_budget_mb, g_cfg->settings.export_shaders ? 1 : 0);
	InitIndices();
}

static bool Supports(IUnknown *dev, const GUID &iid)
{
	IUnknown *p = nullptr;
	if (SUCCEEDED(dev->QueryInterface(iid, (void **)&p)) && p) {
		p->Release();
		return true;
	}
	return false;
}

void HookDevice(IUnknown *unk)
{
	ID3D12Device *dev = nullptr;
	if (FAILED(unk->QueryInterface(__uuidof(ID3D12Device), (void **)&dev)) || !dev)
		return;
	if (!g_device)
		g_device = dev; // raw pointer, deliberately not kept alive
	
	// Track all devices for multi-device support
	{
		std::lock_guard<std::mutex> lk(g_mu);
		if (std::find(g_devices.begin(), g_devices.end(), dev) == g_devices.end()) {
			g_devices.push_back(dev);
			LOG("Tracked device %p (total: %zu)\n", (void *)dev, g_devices.size());
		}
	}
	
	void **vt = *(void ***)dev;
	dev->Release();

	if (hp_CreateCommandList.Has(vt))
		return;
	LOG("Hooking ID3D12Device vtable %p\n", (void *)vt);
	const Settings st = g_cfg ? g_cfg->settings : Settings();
	if (st.hook_commandlists)
		hp_CreateCommandList.Install(vt, g_idx.dev_CreateCommandList, (void *)&Hook_CreateCommandList);
	if (st.hook_pipeline) {
		hp_CreateGraphicsPSO.Install(vt, g_idx.dev_CreateGraphicsPSO, (void *)&Hook_CreateGraphicsPSO);
		hp_CreateComputePSO.Install(vt, g_idx.dev_CreateComputePSO, (void *)&Hook_CreateComputePSO);
	}
	if (st.hook_resources) {
		hp_CreateCommittedResource.Install(vt, g_idx.dev_CreateCommittedResource, (void *)&Hook_CreateCommittedResource);
		hp_CreatePlacedResource.Install(vt, g_idx.dev_CreatePlacedResource, (void *)&Hook_CreatePlacedResource);
		hp_CreateReservedResource.Install(vt, g_idx.dev_CreateReservedResource, (void *)&Hook_CreateReservedResource);
	}
	// Texture / view creation hooks (for texture override support)
	if (st.hook_resources) {
		hp_CreateShaderResourceView.Install(vt, g_idx.dev_CreateShaderResourceView, (void *)&Hook_CreateShaderResourceView);
		hp_CreateUnorderedAccessView.Install(vt, g_idx.dev_CreateUnorderedAccessView, (void *)&Hook_CreateUnorderedAccessView);
		hp_CreateRenderTargetView.Install(vt, g_idx.dev_CreateRenderTargetView, (void *)&Hook_CreateRenderTargetView);
		hp_CreateDepthStencilView.Install(vt, g_idx.dev_CreateDepthStencilView, (void *)&Hook_CreateDepthStencilView);
		hp_CreateRootSignature.Install(vt, g_idx.dev_CreateRootSignature, (void *)&Hook_CreateRootSignature);
	}
	if (st.hook_pipeline && Supports(unk, __uuidof(ID3D12Device2)))
		hp_CreatePipelineState.Install(vt, g_idx.dev2_CreatePipelineState, (void *)&Hook_CreatePipelineState);
	if (Supports(unk, __uuidof(ID3D12Device4))) {
		if (st.hook_commandlists)
			hp_CreateCommandList1.Install(vt, g_idx.dev4_CreateCommandList1, (void *)&Hook_CreateCommandList1);
		if (st.hook_resources)
			hp_CreateCommittedResource1.Install(vt, g_idx.dev4_CreateCommittedResource1, (void *)&Hook_CreateCommittedResource1);
	}
	if (st.hook_resources && Supports(unk, kIID_Device8)) {
		hp_CreateCommittedResource2.Install(vt, g_idx.dev8_CreateCommittedResource2, (void *)&Hook_CreateCommittedResource2);
		hp_CreatePlacedResource1.Install(vt, g_idx.dev8_CreatePlacedResource1, (void *)&Hook_CreatePlacedResource1);
	}
	if (st.hook_resources && Supports(unk, kIID_Device10))
		hp_CreateCommittedResource3.Install(vt, g_idx.dev10_CreateCommittedResource3, (void *)&Hook_CreateCommittedResource3);
	LOG("Device hooks: commandlists=%d pipeline=%d resources=%d release=%d\n", st.hook_commandlists, st.hook_pipeline, st.hook_resources, st.hook_release);
}

} // namespace mig12
