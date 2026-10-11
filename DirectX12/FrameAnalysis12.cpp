// 3DMigoto DX12 - frame analysis.
//
// Produces a "FrameAnalysis-<date>-<time>" folder with a log.txt and per-draw
// vertex / index buffer dumps using the same naming and text formats as the
// DX11 tool, so XXMITools / the 3DMigoto Blender importer can read them:
//   000123-vb0=<hash>-vs=<hash>-ps=<hash>.txt / .buf
//   000123-ib=<hash>-vs=<hash>-ps=<hash>.txt / .buf
#include "State12.h"

#include <cstdio>

namespace mig12 {

static std::mutex g_fa_mu;
static std::atomic<bool> g_fa_active{ false };
static std::atomic<bool> g_fa_requested{ false };
static std::string g_fa_dir;
static FILE *g_fa_log = nullptr;
static uint32_t g_fa_draw = 0;
static std::unordered_map<std::string, std::string> g_fa_dedupe;
static std::set<std::pair<int, uint64_t>> g_fa_shaders;

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
	LOG("Frame analysis finished: %u draw calls written to %s\n", g_fa_draw, g_fa_dir.c_str());
	// Update hooks active flag: still needed if hunting=1 or overrides present
	g_hooks_active = (g_hunting.load() == 1) || (g_cfg && g_cfg->HasTextureOverrides());
	char msg[256];
	snprintf(msg, sizeof(msg), "Frame analysis done: %u draws", g_fa_draw);
	Overlay_SetText(msg);
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

		char base[160];
		snprintf(base, sizeof(base), "%s\\%06u-vb%d=%08x%s", g_fa_dir.c_str(), n, s, d.vb_hash[s], suffix.c_str());
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
		char base[160];
		snprintf(base, sizeof(base), "%s\\%06u-ib=%08x%s", g_fa_dir.c_str(), n, d.ib_hash, suffix.c_str());
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
