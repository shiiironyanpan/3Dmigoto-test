// 3DMigoto DX12 - frame analysis.
//
// Produces a "FrameAnalysis-<date>-<time>" folder with a log.txt and per-draw
// vertex / index buffer dumps using the same naming and text formats as the
// DX11 tool, so XXMITools / the 3DMigoto Blender importer can read them:
//   000123-vb0=<hash>-vs=<hash>-ps=<hash>.txt / .buf
//   000123-ib=<hash>-vs=<hash>-ps=<hash>.txt / .buf
//   000123-t0=<hash>-vs=<hash>-ps=<hash>.dds / .txt (texture dumps)
#include "State12.h"

#include <cstdio>
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace mig12 {

static std::mutex g_fa_mu;
static std::atomic<bool> g_fa_active{ false };
static std::atomic<bool> g_fa_requested{ false };
static std::string g_fa_dir;
static FILE *g_fa_log = nullptr;
static uint32_t g_fa_draw = 0;
static uint32_t g_fa_frame_no = 0;  // Frame number for HOLD mode filename prefix (matching DX11)
static std::unordered_map<std::string, std::string> g_fa_dedupe;
static std::set<std::pair<int, uint64_t>> g_fa_shaders;

// Forward declarations
static void DumpTextures();
static std::string BuildFAPrefix(uint32_t draw_no);
static void WriteDDS(const std::string &path, const void *data, size_t size, const D3D12_RESOURCE_DESC &desc);

void FA_Request()
{
	g_fa_requested = true;
}

bool FA_Active()
{
	return g_fa_active.load();
}

void FA_StartIfRequested()
{
	if (!g_fa_requested.exchange(false) || g_fa_active.load())
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	char name[128];
	snprintf(name, sizeof(name), "FrameAnalysis-%04d-%02d-%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

	std::lock_guard<std::mutex> lk(g_fa_mu);
	g_fa_dir = g_ini_dir + "\\" + name;
	if (!CreateDirectoryW(WidenUtf8(g_fa_dir).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		LOG("ERROR: cannot create frame analysis directory %s\n", g_fa_dir.c_str());
		return;
	}
	g_fa_log = _wfsopen(WidenUtf8(g_fa_dir + "\\log.txt").c_str(), L"w", _SH_DENYNO);
	if (!g_fa_log) {
		LOG("ERROR: cannot create frame analysis log\n");
		return;
	}
	g_fa_draw = 0;
	g_fa_frame_no++;  // Increment frame number for HOLD mode (matching DX11)
	g_fa_dedupe.clear();
	g_fa_shaders.clear();
	fprintf(g_fa_log, "3DMigoto DX12 frame analysis\n");
	g_fa_active = true;
	g_hooks_active = true; // frame analysis needs hooks
	LOG("Frame analysis started: %s\n", g_fa_dir.c_str());
	Overlay_SetText("Frame analysis running...");
}

void FA_Stop()
{
	if (!g_fa_active.exchange(false))
		return;
	std::lock_guard<std::mutex> lk(g_fa_mu);
	if (g_fa_log) {
		fclose(g_fa_log);
		g_fa_log = nullptr;
	}
	// Dump the shaders that were used in this frame
	if (!g_fa_shaders.empty()) {
		std::string sd = g_fa_dir + "\\shaders";
		CreateDirectoryW(WidenUtf8(sd).c_str(), nullptr);
		for (auto &p : g_fa_shaders) {
			std::vector<uint8_t> bytes;
			if (GetShaderBytes(p.second, bytes)) {
				char hn[32];
				snprintf(hn, sizeof(hn), "%016llx", (unsigned long long)p.second);
				WriteFileBytes(sd + "\\" + hn + "-" + kShaderKindName[p.first] + ".bin", bytes.data(), bytes.size());
			}
		}
	}
	// Also dump shaders alongside draw calls (DX11 format)
	for (auto &p : g_fa_shaders) {
		std::vector<uint8_t> bytes;
		if (GetShaderBytes(p.second, bytes)) {
			char hn[32];
			snprintf(hn, sizeof(hn), "%016llx", (unsigned long long)p.second);
			// Use frame 0 prefix for shader dumps (they're not per-draw)
			std::string base = g_fa_dir + "\\000000-shader-" + kShaderKindName[p.first] + "=" + hn;
			WriteFileBytes(base + ".bin", bytes.data(), bytes.size());
		}
	}
	// Dump textures that were bound during the frame
	DumpTextures();
	LOG("Frame analysis finished: %u draw calls written to %s\n", g_fa_draw, g_fa_dir.c_str());
	// Update hooks active flag: still needed if hunting=1 or overrides present
	g_hooks_active = (g_hunting.load() == 1) || (g_cfg && g_cfg->HasTextureOverrides());
	char msg[256];
	snprintf(msg, sizeof(msg), "Frame analysis done: %u draws", g_fa_draw);
	Overlay_SetText(msg);
}

static void DumpTextures()
{
	if (!g_cfg || !g_cfg->settings.export_shaders || !g_device)
		return;

	// Collect all tracked resources that have SRV created
	std::vector<ResEx*> textures_to_dump;
	{
		std::lock_guard<std::mutex> lk(g_mu);
		for (auto &kv : g_res) {
			ResEx *ri = kv.second.get();
			if (ri->has_srv && (ri->mapped || ri->has_shadow) && ri->dim != D3D12_RESOURCE_DIMENSION_BUFFER) {
				textures_to_dump.push_back(ri);
			}
		}
	}

	if (textures_to_dump.empty())
		return;

	LOG("Dumping %zu textures...\n", textures_to_dump.size());

	// Create a copy command queue for texture readback
	ID3D12CommandQueue *copy_queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
	queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	HRESULT hr = g_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&copy_queue));
	if (FAILED(hr) || !copy_queue) {
		LOG("Failed to create copy queue for texture dump (hr=0x%08x)\n", (unsigned)hr);
		return;
	}

	// Create a fence for synchronization
	ID3D12Fence *fence = nullptr;
	hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	if (FAILED(hr) || !fence) {
		LOG("Failed to create fence for texture dump (hr=0x%08x)\n", (unsigned)hr);
		copy_queue->Release();
		return;
	}
	HANDLE fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	// Create a command allocator and list for copy commands
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *cmd_list = nullptr;
	hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&alloc));
	if (FAILED(hr)) {
		LOG("Failed to create command allocator for texture dump\n");
		CloseHandle(fence_event);
		fence->Release();
		copy_queue->Release();
		return;
	}
	hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, alloc, nullptr, IID_PPV_ARGS(&cmd_list));
	if (FAILED(hr)) {
		LOG("Failed to create command list for texture dump\n");
		alloc->Release();
		CloseHandle(fence_event);
		fence->Release();
		copy_queue->Release();
		return;
	}

	// Record copy commands for all textures
	for (ResEx *ri : textures_to_dump) {
		if (!ri->res)
			continue;

		D3D12_RESOURCE_DESC desc = ri->res->GetDesc();
		if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
			continue;

		// Calculate required readback buffer size
		UINT64 readback_size = 0;
		g_device->GetCopyableFootprints(&desc, 0, desc.MipLevels, 0, nullptr, nullptr, nullptr, &readback_size);

		// Create readback buffer
		D3D12_HEAP_PROPERTIES readback_heap = {};
		readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
		readback_heap.CreationNodeMask = 1;
		readback_heap.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC readback_desc = {};
		readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		readback_desc.Width = readback_size;
		readback_desc.Height = 1;
		readback_desc.DepthOrArraySize = 1;
		readback_desc.MipLevels = 1;
		readback_desc.Format = DXGI_FORMAT_UNKNOWN;
		readback_desc.SampleDesc.Count = 1;
		readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		ID3D12Resource *readback_buffer = nullptr;
		hr = g_device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_buffer));
		if (FAILED(hr) || !readback_buffer) {
			LOG("Failed to create readback buffer for texture (hr=0x%08x)\n", (unsigned)hr);
			continue;
		}

		// Transition texture to COPY_SOURCE
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = ri->res;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cmd_list->ResourceBarrier(1, &barrier);

		// Copy texture to readback buffer
				D3D12_TEXTURE_COPY_LOCATION dst_location = {};
				dst_location.pResource = readback_buffer;
				dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dst_location.PlacedFootprint = {};

				D3D12_TEXTURE_COPY_LOCATION src_location = {};
				src_location.pResource = ri->res;
				src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				src_location.SubresourceIndex = 0;

				cmd_list->CopyTextureRegion(
					&dst_location,
					0, 0, 0,
					&src_location,
					nullptr
				);

		// Transition back
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		cmd_list->ResourceBarrier(1, &barrier);

		// Store readback buffer for later processing
		// We'll process after executing the command list
		// For simplicity, we'll execute and wait after each texture
		hr = cmd_list->Close();
		if (FAILED(hr)) {
			readback_buffer->Release();
			continue;
		}

		// Execute the copy command
		ID3D12CommandList *lists[] = { cmd_list };
		copy_queue->ExecuteCommandLists(1, lists);

		// Signal fence and wait
		UINT64 fence_value = 1;
		hr = copy_queue->Signal(fence, fence_value);
		if (SUCCEEDED(hr)) {
			hr = fence->SetEventOnCompletion(fence_value, fence_event);
			if (SUCCEEDED(hr)) {
				WaitForSingleObject(fence_event, INFINITE);
			}
		}

		// Map readback buffer and write DDS
		void *mapped = nullptr;
		D3D12_RANGE read_range = { 0, readback_size };
		hr = readback_buffer->Map(0, &read_range, &mapped);
		if (SUCCEEDED(hr) && mapped) {
			// Write DDS file
			char hn[64];
						snprintf(hn, sizeof(hn), "%08x", (unsigned int)(ri->res->GetGPUVirtualAddress() & 0xFFFFFFFF)); // Simple hash from VA
			// Use same filename format as draw calls: framedraw-t0=hash.dds
			std::string prefix = BuildFAPrefix(0); // Frame 0 for textures
			std::string base = g_fa_dir + "\\" + prefix + "t0=" + hn;
			WriteDDS(base + ".dds", mapped, readback_size, desc);
			readback_buffer->Unmap(0, nullptr);
		}

		readback_buffer->Release();
		fence->Signal(0); // Reset fence for next texture
		cmd_list->Reset(alloc, nullptr);
	}

	// Cleanup
	cmd_list->Release();
	alloc->Release();
	CloseHandle(fence_event);
	fence->Release();
	copy_queue->Release();
}

static void WriteDDS(const std::string &path, const void *data, size_t size, const D3D12_RESOURCE_DESC &desc)
{
	// Simple DDS header writer
	// For a complete implementation, we'd need to properly format the DDS header
	// with the correct DXGI format, dimensions, mip levels, etc.
	// This is a placeholder that writes raw data with a minimal DDS header
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return;

	// Minimal DDS header (128 bytes)
	struct DDSHeader {
		uint32_t dwMagic;
		uint32_t dwSize;
		uint32_t dwFlags;
		uint32_t dwHeight;
		uint32_t dwWidth;
		uint32_t dwPitchOrLinearSize;
		uint32_t dwDepth;
		uint32_t dwMipMapCount;
		uint32_t dwReserved1[11];
		struct {
			uint32_t dwSize;
			uint32_t dwFlags;
			uint32_t dwFourCC;
			uint32_t dwRGBBitCount;
			uint32_t dwRBitMask;
			uint32_t dwGBitMask;
			uint32_t dwBBitMask;
			uint32_t dwABitMask;
		} ddspf;
		uint32_t dwCaps;
		uint32_t dwCaps2;
		uint32_t dwCaps3;
		uint32_t dwCaps4;
		uint32_t dwReserved2;
	} hdr = {};

	hdr.dwMagic = 0x20534444; // "DDS "
	hdr.dwSize = 124;
	hdr.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000; // DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
	hdr.dwHeight = (uint32_t)desc.Height;
	hdr.dwWidth = (uint32_t)desc.Width;
	hdr.dwDepth = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? (uint32_t)desc.DepthOrArraySize : 1;
	hdr.dwMipMapCount = desc.MipLevels;
	hdr.dwPitchOrLinearSize = (uint32_t)size;
	hdr.ddspf.dwSize = 32;
	hdr.ddspf.dwFlags = 0x4; // DDPF_FOURCC
	hdr.ddspf.dwFourCC = 0; // Would need proper format mapping
	hdr.dwCaps = 0x1000; // DDSCAPS_TEXTURE

	fwrite(&hdr, sizeof(hdr), 1, f);
	fwrite(data, size, 1, f);
	fclose(f);
}

// Writes (or hard links when identical content was already written) a file.
static void WriteDedup(const std::string &path, const std::string &key, const void *data, size_t n)
{
	bool dedupe = g_cfg ? g_cfg->settings.fa_dedupe : true;
	if (dedupe) {
		auto it = g_fa_dedupe.find(key);
		if (it != g_fa_dedupe.end()) {
			if (CreateHardLinkW(WidenUtf8(path).c_str(), WidenUtf8(it->second).c_str(), nullptr))
				return;
		}
	}
	if (WriteFileBytes(path, data, n) && dedupe)
		g_fa_dedupe[key] = path;
}

// Frame analysis frame number (for HOLD mode filename prefix, matching DX11)
static uint32_t g_fa_frame_no = 0;

static std::string ShaderSuffix(const PsoInfo *pso)
{
	std::string s;
	if (!pso)
		return s;
	static const int order[] = { SK_VS, SK_HS, SK_DS, SK_GS, SK_PS, SK_CS };
	for (int k : order) {
		if (!pso->hash[k])
			continue;
		char b[64];
		snprintf(b, sizeof(b), "-%s=%016llx", kShaderKindName[k], (unsigned long long)pso->hash[k]);
		s += b;
		g_fa_shaders.insert(std::make_pair(k, pso->hash[k]));
	}
	return s;
}

// Build filename prefix matching DX11 format: [frame.]drawcall-
// When HOLD is enabled (fa_dedupe), prefix with frame number like DX11
static std::string BuildFAPrefix(uint32_t draw_no)
{
	std::string prefix;
	if (g_cfg && g_cfg->settings.fa_dedupe) {
		char b[32];
		snprintf(b, sizeof(b), "%u.", g_fa_frame_no);
		prefix = b;
	}
	char b[32];
	snprintf(b, sizeof(b), "%06i-", (int)draw_no);
	prefix += b;
	return prefix;
}

static int EffectiveTopology(const FADraw &d)
{
	if (d.topology)
		return d.topology;
	if (d.pso) {
		switch (d.pso->topology_type) {
		case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT: return 1;
		case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE: return 2;
		case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE: return 4;
		default: break;
		}
	}
	return 0;
}

void FA_OnDraw(const FADraw &d)
{
	std::lock_guard<std::mutex> lk(g_fa_mu);
	if (!g_fa_active.load() || !g_fa_log)
		return;
	uint32_t n = ++g_fa_draw;
	const std::string suffix = ShaderSuffix(d.pso);
	const int topo = EffectiveTopology(d);
	const char *topo_name = topology_name(topo);
	const std::vector<IAElement> empty_layout;
	const std::vector<IAElement> &layout = d.pso ? d.pso->layout : empty_layout;

	// ---- fetch index data first: it tells us how many vertices an indexed draw touches ---
	std::vector<uint8_t> ib_bytes;
	bool have_ib = false;
	uint32_t vertex_first = 0, vertex_count = 0;
	if (d.indexed) {
		vertex_first = d.base_vertex > 0 ? (uint32_t)d.base_vertex : 0;
		if (d.ib_set && FetchBufferBytes(d.ib.BufferLocation, d.ib.SizeInBytes, ib_bytes)) {
			have_ib = true;
			int fmt = (int)d.ib.Format;
			vertex_count = max_index_plus_one(ib_bytes.data(), ib_bytes.size(), fmt, d.start, d.a);
		}
	} else {
		vertex_first = d.start;
		vertex_count = d.a;
	}

	// ---- bindings in the log (before the draw, same draw number) --------------------------
	int top_slot = -1;
	for (int s = 0; s < 16; s++)
		if (d.vb_set[s] && d.vb_hash[s])
			top_slot = s;
	if (top_slot >= 0) {
		fprintf(g_fa_log, "%06u IASetVertexBuffers(StartSlot:0, NumBuffers:%d, ppVertexBuffers:0x0, pStrides:0x0, pOffsets:0x0)\n", n, top_slot + 1);
		for (int s = 0; s <= top_slot; s++) {
			if (!d.vb_set[s])
				continue;
			fprintf(g_fa_log, "       %d: resource=0x%016llX hash=%08x\n", s, (unsigned long long)d.vb[s].BufferLocation, d.vb_hash[s]);
		}
	}
	if (d.ib_set) {
		fprintf(g_fa_log, "%06u IASetIndexBuffer(pIndexBuffer:0x%016llX, Format:%u, Offset:0)\n", n, (unsigned long long)d.ib.BufferLocation, (unsigned)d.ib.Format);
		fprintf(g_fa_log, "       resource=0x%016llX hash=%08x\n", (unsigned long long)d.ib.BufferLocation, d.ib_hash);
	}
	if (d.pso) {
		fprintf(g_fa_log, "%06u [DX12] pipeline%s\n", n, suffix.c_str());
	}
	if (d.indexed)
		fprintf(g_fa_log, "%06u DrawIndexedInstanced(IndexCountPerInstance:%u, InstanceCount:%u, StartIndexLocation:%u, BaseVertexLocation:%i, StartInstanceLocation:%u)\n",
			n, d.a, d.instance_count, d.start, d.base_vertex, d.start_instance);
	else
		fprintf(g_fa_log, "%06u DrawInstanced(VertexCountPerInstance:%u, InstanceCount:%u, StartVertexLocation:%u, StartInstanceLocation:%u)\n",
			n, d.a, d.instance_count, d.start, d.start_instance);

	// ---- vertex buffers ----------------------------------------------------------------------
	for (int s = 0; s < 16; s++) {
		if (!d.vb_set[s] || !d.vb[s].SizeInBytes)
			continue;
		if (d.pso && !layout.empty() && !(d.pso->used_slots >> s & 1))
			continue;
		std::vector<uint8_t> bytes;
		if (!FetchBufferBytes(d.vb[s].BufferLocation, d.vb[s].SizeInBytes, bytes)) {
			fprintf(g_fa_log, "%06u # vb%d: no CPU copy of this buffer is available (GPU generated or uploaded in an unsupported way)\n", n, s);
			continue;
		}
		bool per_inst = false;
		for (auto &e : layout)
			if ((int)e.slot == s && e.slot_class == 1)
				per_inst = true;

		std::string prefix = BuildFAPrefix(n);
		char base[160];
		snprintf(base, sizeof(base), "%s\\%svb%d=%08x%s", g_fa_dir.c_str(), prefix.c_str(), s, d.vb_hash[s], suffix.c_str());
		char key[200];
		snprintf(key, sizeof(key), "vb|%08x|%llu", d.vb_hash[s], (unsigned long long)bytes.size());
		WriteDedup(std::string(base) + ".buf", key, bytes.data(), bytes.size());

		std::string txt = dump_vb_txt(bytes.data(), bytes.size(), s, d.vb[s].StrideInBytes, 0, vertex_first, vertex_count,
			layout, topo_name, per_inst ? d.start_instance : 0, per_inst ? d.instance_count : 0);
		char tkey[256];
		snprintf(tkey, sizeof(tkey), "vbt|%08x|%llu|%u|%u|%u|%d|%u", d.vb_hash[s], (unsigned long long)bytes.size(),
			d.vb[s].StrideInBytes, vertex_first, vertex_count, topo, d.pso ? (unsigned)d.pso->hash[SK_VS] : 0u);
		// the layout comes from the vertex shader's PSO; include a hash of it in the dedupe key
		uint32_t lh = 0;
		for (auto &e : layout) {
			lh = crc32c(lh, e.semantic.data(), e.semantic.size());
			uint32_t v[4] = { e.semantic_index, (uint32_t)e.format, e.slot, e.aligned_offset };
			lh = crc32c(lh, v, sizeof(v));
		}
		char full[320];
		snprintf(full, sizeof(full), "%s|%08x", tkey, lh);
		WriteDedup(std::string(base) + ".txt", full, txt.data(), txt.size());
	}

	// ---- index buffer -----------------------------------------------------------------------------
	if (d.indexed && d.ib_set && have_ib) {
		std::string prefix = BuildFAPrefix(n);
		char base[160];
		snprintf(base, sizeof(base), "%s\\%sib=%08x%s", g_fa_dir.c_str(), prefix.c_str(), d.ib_hash, suffix.c_str());
		char key[200];
		snprintf(key, sizeof(key), "ib|%08x|%llu", d.ib_hash, (unsigned long long)ib_bytes.size());
		WriteDedup(std::string(base) + ".buf", key, ib_bytes.data(), ib_bytes.size());
		std::string txt = dump_ib_txt(ib_bytes.data(), ib_bytes.size(), (int)d.ib.Format, 0, d.start, d.a, topo_name);
		char tkey[200];
		snprintf(tkey, sizeof(tkey), "ibt|%08x|%llu|%u|%u|%d", d.ib_hash, (unsigned long long)ib_bytes.size(), d.start, d.a, topo);
		WriteDedup(std::string(base) + ".txt", tkey, txt.data(), txt.size());
	} else if (d.indexed && d.ib_set) {
		fprintf(g_fa_log, "%06u # ib: no CPU copy of this buffer is available\n", n);
	}
}

} // namespace mig12
