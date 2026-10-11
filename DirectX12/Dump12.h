// 3DMigoto DX12 - DXGI format helpers and vertex/index buffer text dumpers.
// Portable (no Windows / D3D headers) so the output format can be unit tested.
// The text produced here deliberately mirrors the DX11 FrameAnalysis output
// that XXMITools parses (vb txt / ib txt files).
#pragma once
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mig12 {

struct FmtInfo {
	int id;
	const char *name; // without DXGI_FORMAT_ prefix
	int comps;
	int comp_bytes;
	char kind; // f float32, h float16, u uint, s sint, n unorm, N snorm, t typeless (hex), x opaque (hex dump)
};

inline const FmtInfo *fmt_lookup(int id)
{
	static const FmtInfo table[] = {
		{ 1, "R32G32B32A32_TYPELESS", 4, 4, 't' },
		{ 2, "R32G32B32A32_FLOAT", 4, 4, 'f' },
		{ 3, "R32G32B32A32_UINT", 4, 4, 'u' },
		{ 4, "R32G32B32A32_SINT", 4, 4, 's' },
		{ 5, "R32G32B32_TYPELESS", 3, 4, 't' },
		{ 6, "R32G32B32_FLOAT", 3, 4, 'f' },
		{ 7, "R32G32B32_UINT", 3, 4, 'u' },
		{ 8, "R32G32B32_SINT", 3, 4, 's' },
		{ 9, "R16G16B16A16_TYPELESS", 4, 2, 't' },
		{ 10, "R16G16B16A16_FLOAT", 4, 2, 'h' },
		{ 11, "R16G16B16A16_UNORM", 4, 2, 'n' },
		{ 12, "R16G16B16A16_UINT", 4, 2, 'u' },
		{ 13, "R16G16B16A16_SNORM", 4, 2, 'N' },
		{ 14, "R16G16B16A16_SINT", 4, 2, 's' },
		{ 15, "R32G32_TYPELESS", 2, 4, 't' },
		{ 16, "R32G32_FLOAT", 2, 4, 'f' },
		{ 17, "R32G32_UINT", 2, 4, 'u' },
		{ 18, "R32G32_SINT", 2, 4, 's' },
		{ 24, "R10G10B10A2_UNORM", 1, 4, 'x' },
		{ 25, "R10G10B10A2_UINT", 1, 4, 'x' },
		{ 26, "R11G11B10_FLOAT", 1, 4, 'x' },
		{ 27, "R8G8B8A8_TYPELESS", 4, 1, 't' },
		{ 28, "R8G8B8A8_UNORM", 4, 1, 'n' },
		{ 29, "R8G8B8A8_UNORM_SRGB", 4, 1, 'n' },
		{ 30, "R8G8B8A8_UINT", 4, 1, 'u' },
		{ 31, "R8G8B8A8_SNORM", 4, 1, 'N' },
		{ 32, "R8G8B8A8_SINT", 4, 1, 's' },
		{ 33, "R16G16_TYPELESS", 2, 2, 't' },
		{ 34, "R16G16_FLOAT", 2, 2, 'h' },
		{ 35, "R16G16_UNORM", 2, 2, 'n' },
		{ 36, "R16G16_UINT", 2, 2, 'u' },
		{ 37, "R16G16_SNORM", 2, 2, 'N' },
		{ 38, "R16G16_SINT", 2, 2, 's' },
		{ 39, "R32_TYPELESS", 1, 4, 't' },
		{ 40, "D32_FLOAT", 1, 4, 'f' },
		{ 41, "R32_FLOAT", 1, 4, 'f' },
		{ 42, "R32_UINT", 1, 4, 'u' },
		{ 43, "R32_SINT", 1, 4, 's' },
		{ 48, "R8G8_TYPELESS", 2, 1, 't' },
		{ 49, "R8G8_UNORM", 2, 1, 'n' },
		{ 50, "R8G8_UINT", 2, 1, 'u' },
		{ 51, "R8G8_SNORM", 2, 1, 'N' },
		{ 52, "R8G8_SINT", 2, 1, 's' },
		{ 53, "R16_TYPELESS", 1, 2, 't' },
		{ 54, "R16_FLOAT", 1, 2, 'h' },
		{ 55, "D16_UNORM", 1, 2, 'n' },
		{ 56, "R16_UNORM", 1, 2, 'n' },
		{ 57, "R16_UINT", 1, 2, 'u' },
		{ 58, "R16_SNORM", 1, 2, 'N' },
		{ 59, "R16_SINT", 1, 2, 's' },
		{ 60, "R8_TYPELESS", 1, 1, 't' },
		{ 61, "R8_UNORM", 1, 1, 'n' },
		{ 62, "R8_UINT", 1, 1, 'u' },
		{ 63, "R8_SNORM", 1, 1, 'N' },
		{ 64, "R8_SINT", 1, 1, 's' },
		{ 87, "B8G8R8A8_UNORM", 4, 1, 'n' },
		{ 88, "B8G8R8X8_UNORM", 4, 1, 'n' },
		{ 91, "B8G8R8A8_UNORM_SRGB", 4, 1, 'n' },
		{ 93, "B8G8R8X8_UNORM_SRGB", 4, 1, 'n' },
	};
	for (const FmtInfo &f : table)
		if (f.id == id)
			return &f;
	return nullptr;
}

inline std::string fmt_name(int id)
{
	const FmtInfo *f = fmt_lookup(id);
	if (f)
		return f->name;
	return "UNKNOWN_" + std::to_string(id);
}

// Size in bytes of one element of the format (0 if unknown)
inline unsigned fmt_size(int id)
{
	const FmtInfo *f = fmt_lookup(id);
	return f ? (unsigned)(f->comps * f->comp_bytes) : 0;
}

// Alignment required for AlignedByteOffset == APPEND
inline unsigned fmt_alignment(int id)
{
	const FmtInfo *f = fmt_lookup(id);
	return f ? (unsigned)f->comp_bytes : 0;
}

inline int fmt_from_name(std::string s)
{
	for (auto &c : s)
		c = (char)toupper((unsigned char)c);
	if (s.compare(0, 12, "DXGI_FORMAT_") == 0)
		s = s.substr(12);
	for (int i = 0; i < 256; i++) {
		const FmtInfo *f = fmt_lookup(i);
		if (f && s == f->name)
			return i;
	}
	return 0;
}

inline float half_to_float(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1f;
	uint32_t man = h & 0x3ff;
	uint32_t out;
	if (exp == 0) {
		if (man == 0) {
			out = sign;
		} else {
			// subnormal
			exp = 127 - 15 + 1;
			while (!(man & 0x400)) {
				man <<= 1;
				exp--;
			}
			man &= 0x3ff;
			out = sign | (exp << 23) | (man << 13);
		}
	} else if (exp == 31) {
		out = sign | 0x7f800000u | (man << 13);
	} else {
		out = sign | ((exp + 127 - 15) << 23) | (man << 13);
	}
	float f;
	memcpy(&f, &out, 4);
	return f;
}

inline std::string str_printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

// Prints one vertex element the same way DX11 3DMigoto does
inline std::string format_element(int id, const uint8_t *b)
{
	const FmtInfo *f = fmt_lookup(id);
	std::string out;
	if (!f || f->kind == 'x') {
		unsigned n = f ? f->comps * f->comp_bytes : 0;
		for (unsigned i = 0; i < n; i++)
			out += str_printf("%02x", b[i]);
		return out;
	}
	for (int c = 0; c < f->comps; c++) {
		if (c)
			out += ", ";
		const uint8_t *p = b + c * f->comp_bytes;
		switch (f->kind) {
		case 'f': {
			float v;
			memcpy(&v, p, 4);
			out += str_printf("%.9g", v);
			break;
		}
		case 'h': {
			uint16_t v;
			memcpy(&v, p, 2);
			out += str_printf("%.9g", half_to_float(v));
			break;
		}
		case 'u':
			if (f->comp_bytes == 4) {
				uint32_t v;
				memcpy(&v, p, 4);
				out += str_printf("%u", v);
			} else if (f->comp_bytes == 2) {
				uint16_t v;
				memcpy(&v, p, 2);
				out += str_printf("%u", v);
			} else {
				out += str_printf("%u", *p);
			}
			break;
		case 's':
			if (f->comp_bytes == 4) {
				int32_t v;
				memcpy(&v, p, 4);
				out += str_printf("%d", v);
			} else if (f->comp_bytes == 2) {
				int16_t v;
				memcpy(&v, p, 2);
				out += str_printf("%d", v);
			} else {
				out += str_printf("%d", (int)(int8_t)*p);
			}
			break;
		case 'n':
			if (f->comp_bytes == 2) {
				uint16_t v;
				memcpy(&v, p, 2);
				out += str_printf("%.9g", v / 65535.0f);
			} else {
				out += str_printf("%.9g", *p / 255.0f);
			}
			break;
		case 'N':
			if (f->comp_bytes == 2) {
				int16_t v;
				memcpy(&v, p, 2);
				float r = v / 32767.0f;
				out += str_printf("%.9g", r < -1.0f ? -1.0f : r);
			} else {
				float r = (int8_t)*p / 127.0f;
				out += str_printf("%.9g", r < -1.0f ? -1.0f : r);
			}
			break;
		case 't':
		default:
			if (f->comp_bytes == 4) {
				uint32_t v;
				memcpy(&v, p, 4);
				out += str_printf("%08x", v);
			} else if (f->comp_bytes == 2) {
				uint16_t v;
				memcpy(&v, p, 2);
				out += str_printf("%04x", v);
			} else {
				out += str_printf("%02x", *p);
			}
			break;
		}
	}
	return out;
}

// One element of the input layout (D3D12_INPUT_ELEMENT_DESC equivalent)
struct IAElement {
	std::string semantic;
	uint32_t semantic_index = 0;
	int format = 0;
	uint32_t slot = 0;
	uint32_t aligned_offset = 0; // 0xffffffff == append
	int slot_class = 0;          // 0 per-vertex, 1 per-instance
	uint32_t step_rate = 0;
};

static const uint32_t IA_APPEND = 0xffffffffu;

// Resolve APPEND offsets sequentially per input slot
inline std::vector<uint32_t> resolve_offsets(const std::vector<IAElement> &layout)
{
	std::vector<uint32_t> out(layout.size(), 0);
	uint32_t next[32] = {};
	for (size_t i = 0; i < layout.size(); i++) {
		uint32_t slot = layout[i].slot < 32 ? layout[i].slot : 31;
		uint32_t off;
		if (layout[i].aligned_offset != IA_APPEND) {
			off = layout[i].aligned_offset;
		} else {
			off = next[slot];
			unsigned al = fmt_alignment(layout[i].format);
			if (al && off % al)
				off += al - (off % al);
		}
		out[i] = off;
		next[slot] = off + fmt_size(layout[i].format);
	}
	return out;
}

inline std::string dump_ia_layout(const std::vector<IAElement> &layout, int slot, bool *per_vert, bool *per_inst)
{
	std::string s;
	for (size_t i = 0; i < layout.size(); i++) {
		const IAElement &e = layout[i];
		s += str_printf("element[%u]:\n", (unsigned)i);
		s += "  SemanticName: " + e.semantic + "\n";
		s += str_printf("  SemanticIndex: %u\n", e.semantic_index);
		s += "  Format: " + fmt_name(e.format) + "\n";
		s += str_printf("  InputSlot: %u\n", e.slot);
		if (e.aligned_offset == IA_APPEND)
			s += "  AlignedByteOffset: append\n";
		else
			s += str_printf("  AlignedByteOffset: %u\n", e.aligned_offset);
		if (e.slot_class == 0) {
			s += "  InputSlotClass: per-vertex\n";
			if ((int)e.slot == slot)
				*per_vert = true;
		} else {
			s += "  InputSlotClass: per-instance\n";
			if ((int)e.slot == slot)
				*per_inst = true;
		}
		s += str_printf("  InstanceDataStepRate: %u\n", e.step_rate);
	}
	return s;
}

inline std::string dump_vb_elem(const uint8_t *vertex, const std::vector<IAElement> &layout,
		const std::vector<uint32_t> &offs, int slot, unsigned vb_idx, size_t elem, unsigned stride)
{
	const IAElement &e = layout[elem];
	if ((int)e.slot != slot)
		return "";
	uint32_t offset = offs[elem];
	std::string s = str_printf("vb%i[%u]+%03u %s", slot, vb_idx, offset, e.semantic.c_str());
	if (e.semantic_index)
		s += str_printf("%u", e.semantic_index);
	s += ": ";
	s += format_element(e.format, vertex + offset);
	s += "\n";
	unsigned size = fmt_size(e.format);
	if (!size)
		s += "# WARNING: Unknown format size, vertex buffer may be decoded incorrectly\n";
	if (offset + size > stride)
		s += "# WARNING: Offset exceeded stride, vertex buffer may be decoded incorrectly\n";
	return s;
}

// size: number of valid bytes in data. offset/first/count follow DX11 semantics.
inline std::string dump_vb_txt(const uint8_t *data, size_t size, int slot, uint32_t stride,
		uint32_t offset, uint32_t first, uint32_t count, const std::vector<IAElement> &layout,
		const char *topology, uint32_t first_inst, uint32_t inst_count)
{
	std::string s;
	if (offset)
		s += str_printf("offset: %u\n", offset);
	s += str_printf("stride: %u\n", stride);
	if (first || count) {
		s += str_printf("first vertex: %u\n", first);
		s += str_printf("vertex count: %u\n", count);
	}
	if (first_inst || inst_count) {
		s += str_printf("first instance: %u\n", first_inst);
		s += str_printf("instance count: %u\n", inst_count);
	}
	if (topology && *topology)
		s += str_printf("topology: %s\n", topology);

	bool per_vert = false, per_inst = false;
	std::vector<uint32_t> offs;
	if (!layout.empty()) {
		s += dump_ia_layout(layout, slot, &per_vert, &per_inst);
		offs = resolve_offsets(layout);
	}
	if (!stride) {
		s += "# ERROR: Cannot dump vertex buffer with stride=0\n";
		return s;
	}

	uint32_t start = offset / stride + first;
	uint32_t end = (uint32_t)(size / stride);
	if (count && start + count < end)
		end = start + count;

	if (!layout.empty()) {
		if (per_vert) {
			s += "\nvertex-data:\n";
			for (uint32_t v = start; v < end; v++) {
				s += "\n";
				for (size_t e = 0; e < layout.size(); e++) {
					if (layout[e].slot_class != 0)
						continue;
					s += dump_vb_elem(data + (size_t)stride * v, layout, offs, slot, v - start, e, stride);
				}
			}
		}
		if (per_inst) {
			s += "\ninstance-data:\n";
			uint32_t istart = offset / stride + first_inst;
			uint32_t iend = (uint32_t)(size / stride);
			if (inst_count && istart + inst_count < iend)
				iend = istart + inst_count;
			for (uint32_t i = istart; i < iend; i++) {
				s += "\n";
				for (size_t e = 0; e < layout.size(); e++) {
					if (layout[e].slot_class != 1)
						continue;
					uint32_t idx = i;
					if (layout[e].step_rate)
						idx = (i - istart) / layout[e].step_rate + istart;
					s += dump_vb_elem(data + (size_t)stride * idx, layout, offs, slot, idx - istart, e, stride);
				}
			}
		}
	} else {
		// Unknown layout: dump as raw 32-bit words
		s += "\nvertex-data:\n";
		for (uint32_t v = start; v < end; v++) {
			s += "\n";
			const uint8_t *p = data + (size_t)stride * v;
			for (uint32_t j = 0; j + 4 <= stride; j += 4) {
				uint32_t u;
				float f;
				memcpy(&u, p + j, 4);
				memcpy(&f, p + j, 4);
				s += str_printf("vb%i[%u]+%03u: %08x (%.9g)\n", slot, v - start, j, u, f);
			}
		}
	}
	return s;
}

inline std::string dump_ib_txt(const uint8_t *data, size_t size, int format, uint32_t offset,
		uint32_t first, uint32_t count, const char *topology)
{
	std::string s;
	s += str_printf("byte offset: %u\n", offset);
	if (first || count) {
		s += str_printf("first index: %u\n", first);
		s += str_printf("index count: %u\n", count);
	}
	if (topology && *topology)
		s += str_printf("topology: %s\n", topology);

	int grouping = 1;
	if (topology) {
		if (!strcmp(topology, "linelist"))
			grouping = 2;
		else if (!strcmp(topology, "trianglelist"))
			grouping = 3;
	}

	if (format == 57) {
		s += "format: DXGI_FORMAT_R16_UINT\n";
		uint32_t start = offset / 2 + first;
		uint32_t end = (uint32_t)(size / 2);
		if (count && start + count < end)
			end = start + count;
		for (uint32_t i = start; i < end; i++) {
			uint16_t v;
			memcpy(&v, data + (size_t)i * 2, 2);
			s += ((i - start) % grouping == 0) ? "\n" : " ";
			s += str_printf("%u", v);
		}
		s += "\n";
	} else if (format == 42) {
		s += "format: DXGI_FORMAT_R32_UINT\n";
		uint32_t start = offset / 4 + first;
		uint32_t end = (uint32_t)(size / 4);
		if (count && start + count < end)
			end = start + count;
		for (uint32_t i = start; i < end; i++) {
			uint32_t v;
			memcpy(&v, data + (size_t)i * 4, 4);
			s += ((i - start) % grouping == 0) ? "\n" : " ";
			s += str_printf("%u", v);
		}
		s += "\n";
	} else {
		s += str_printf("format %u is illegal\n", (unsigned)format);
	}
	return s;
}

// Scans an index range for the highest referenced vertex (+1), used to work out
// how many vertices an indexed draw really touches.
inline uint32_t max_index_plus_one(const uint8_t *data, size_t size, int format, uint32_t first, uint32_t count)
{
	uint32_t mx = 0;
	if (format == 57) {
		size_t n = size / 2;
		for (size_t i = first; i < n && i < (size_t)first + count; i++) {
			uint16_t v;
			memcpy(&v, data + i * 2, 2);
			if ((uint32_t)v + 1 > mx)
				mx = (uint32_t)v + 1;
		}
	} else if (format == 42) {
		size_t n = size / 4;
		for (size_t i = first; i < n && i < (size_t)first + count; i++) {
			uint32_t v;
			memcpy(&v, data + i * 4, 4);
			if (v + 1 > mx)
				mx = v + 1;
		}
	}
	return mx;
}

inline const char *topology_name(int t)
{
	switch (t) {
	case 1: return "pointlist";
	case 2: return "linelist";
	case 3: return "linestrip";
	case 4: return "trianglelist";
	case 5: return "trianglestrip";
	case 10: return "linelist_adj";
	case 11: return "linestrip_adj";
	case 12: return "trianglelist_adj";
	case 13: return "trianglestrip_adj";
	}
	if (t >= 33 && t <= 64)
		return "patchlist";
	return "";
}

} // namespace mig12
