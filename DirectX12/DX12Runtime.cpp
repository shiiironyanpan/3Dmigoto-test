#include "DX12Runtime.h"
#include "DX12Hooks.h"
#include "../log.h"
#include "DX12Format.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace {

static std::wstring TrimW(std::wstring s)
{
    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

static std::wstring LowerW(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), towlower);
    return s;
}

static bool IsTrueW(const std::wstring& s)
{
    std::wstring v = LowerW(TrimW(s));
    return v == L"1" || v == L"true" || v == L"yes" || v == L"on";
}

static UINT64 ParseHex64(const std::wstring& text)
{
    std::wstring s = TrimW(text);
    if (s.size() > 2 && s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s = s.substr(2);
    wchar_t* end = nullptr;
    return _wcstoui64(s.c_str(), &end, 16);
}

static bool ParseUInt(const std::wstring& text, UINT* value)
{
    if (!value) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(TrimW(text).c_str(), &end, 0);
    if (!end || *end) return false;
    *value = static_cast<UINT>(v);
    return true;
}

static bool ParseInt(const std::wstring& text, INT* value)
{
    if (!value) return false;
    wchar_t* end = nullptr;
    long v = wcstol(TrimW(text).c_str(), &end, 0);
    if (!end || *end) return false;
    *value = static_cast<INT>(v);
    return true;
}

static std::wstring IniValue(const std::wstring& ini, const std::wstring& section, const wchar_t* key)
{
    wchar_t buf[8192]{};
    DWORD n = GetPrivateProfileStringW(section.c_str(), key, L"", buf, ARRAYSIZE(buf), ini.c_str());
    return std::wstring(buf, n);
}

static std::vector<std::wstring> IniSections(const std::wstring& ini)
{
    std::vector<std::wstring> result;
    std::vector<wchar_t> buf(65536);
    DWORD n = GetPrivateProfileSectionNamesW(buf.data(), static_cast<DWORD>(buf.size()), ini.c_str());
    if (!n || n >= buf.size() - 1) return result;
    const wchar_t* p = buf.data();
    while (*p) {
        result.push_back(p);
        p += wcslen(p) + 1;
    }
    return result;
}

static std::wstring SectionPrefix(const std::wstring& s, const wchar_t* prefix)
{
    std::wstring a = LowerW(s), b = LowerW(prefix);
    if (a.compare(0, b.size(), b) == 0) return s.substr(b.size());
    return L"";
}

static DXGI_FORMAT ParseDXGIFormatW(const std::wstring& text)
{
    return DX12ParseFormat(text.c_str());
}

static bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& bytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart <= 0x7fffffff;
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(h);
    return ok;
}

static std::wstring ReplaceExtension(const std::wstring& path, const wchar_t* ext)
{
    size_t p = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    if (p == std::wstring::npos || (slash != std::wstring::npos && p < slash)) return path + ext;
    return path.substr(0, p) + ext;
}

static std::wstring FileStem(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) dot = path.size();
    return path.substr(slash == std::wstring::npos ? 0 : slash + 1, dot - (slash == std::wstring::npos ? 0 : slash + 1));
}

static bool PrefixCI(const std::wstring& s, const wchar_t* p)
{
    std::wstring a = LowerW(s), b = LowerW(p);
    return a.size() >= b.size() && a.compare(0, b.size(), b) == 0;
}

static bool IsBufferResourceSection(const std::wstring& ini, const std::wstring& section)
{
    return LowerW(TrimW(IniValue(ini, section, L"type"))) == L"buffer" ||
           !IniValue(ini, section, L"stride").empty() ||
           !IniValue(ini, section, L"format").empty();
}

} // namespace

DX12Runtime& DX12Runtime::Instance()
{
    static DX12Runtime instance;
    return instance;
}

extern "C" DX12Runtime* GetDX12Runtime()
{
    return &DX12Runtime::Instance();
}

std::wstring DX12Runtime::BasePath() const
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GetDX12Runtime), &module);
    GetModuleFileNameW(module, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    return path;
}

std::wstring DX12Runtime::IniPath() const
{
    return BasePath() + L"d3dx.ini";
}

std::wstring DX12Runtime::ResolvePath(const std::wstring& path) const
{
    std::wstring p = TrimW(path);
    if (p.empty()) return p;
    if ((p.size() > 1 && p[1] == L':') || (p.size() > 1 && p[0] == L'\\' && p[1] == L'\\')) return p;
    return BasePath() + p;
}

UINT64 DX12Runtime::HashShader(const void* data, SIZE_T size) const
{
    // 3DMigoto's shader hash is the unseeded 64-bit FNV-1 hash.
    const BYTE* p = static_cast<const BYTE*>(data);
    UINT64 h = 0xcbf29ce484222325ULL;
    for (SIZE_T i = 0; i < size; ++i) {
        h *= 0x100000001b3ULL;
        h ^= p[i];
    }
    return h;
}

UINT32 DX12Runtime::CRC32C(const BYTE* data, size_t size) const
{
    static UINT32 table[256];
    static INIT_ONCE init = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        for (UINT32 i = 0; i < 256; ++i) {
            UINT32 r = i;
            for (int j = 0; j < 8; ++j) r = (r >> 1) ^ ((r & 1) ? 0x82f63b78U : 0);
            table[i] = r;
        }
        return TRUE;
    }, nullptr, nullptr);
    UINT32 crc = 0xffffffffU;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

std::string DX12Runtime::Hex32(UINT32 value) const
{
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(8) << value;
    return s.str();
}

std::string DX12Runtime::Hex64(UINT64 value) const
{
    std::ostringstream s;
    s << std::hex << std::setfill('0') << std::setw(16) << value;
    return s.str();
}

UINT DX12Runtime::FormatSize(DXGI_FORMAT f) const
{
    return DX12FormatSize(f);
}

const char* DX12Runtime::FormatName(DXGI_FORMAT f) const
{
    return DX12FormatName(f);
}

const char* DX12Runtime::TopologyName(D3D12_PRIMITIVE_TOPOLOGY t) const
{
    switch (t) {
    case D3D_PRIMITIVE_TOPOLOGY_UNDEFINED: return "undefined";
    case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: return "pointlist";
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST: return "linelist";
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: return "linestrip";
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return "trianglelist";
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return "trianglestrip";
    case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return "linelist_adj";
    case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return "linestrip_adj";
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return "trianglelist_adj";
    case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return "trianglestrip_adj";
    default: break;
    }
    static char patch[64];
    if (t >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) {
        sprintf_s(patch, "%u_control_point_patchlist", static_cast<unsigned>(t - D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1));
        return patch;
    }
    return "invalid";
}

UINT64 DX12Runtime::HashPipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc) const
{
    if (!desc) return 0;
    UINT64 h = 0xcbf29ce484222325ULL;
    auto mix = [&h](const void* data, size_t n) {
        const BYTE* p = static_cast<const BYTE*>(data);
        for (size_t i = 0; i < n; ++i) { h *= 0x100000001b3ULL; h ^= p[i]; }
    };
    if (desc->VS.pShaderBytecode) mix(desc->VS.pShaderBytecode, desc->VS.BytecodeLength);
    if (desc->HS.pShaderBytecode) mix(desc->HS.pShaderBytecode, desc->HS.BytecodeLength);
    if (desc->DS.pShaderBytecode) mix(desc->DS.pShaderBytecode, desc->DS.BytecodeLength);
    if (desc->GS.pShaderBytecode) mix(desc->GS.pShaderBytecode, desc->GS.BytecodeLength);
    if (desc->PS.pShaderBytecode) mix(desc->PS.pShaderBytecode, desc->PS.BytecodeLength);
    if (desc->InputLayout.pInputElementDescs && desc->InputLayout.NumElements)
        for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& e = desc->InputLayout.pInputElementDescs[i];
            if (e.SemanticName) mix(e.SemanticName, strlen(e.SemanticName));
            mix(&e.SemanticIndex, sizeof(e.SemanticIndex));
            mix(&e.Format, sizeof(e.Format));
            mix(&e.InputSlot, sizeof(e.InputSlot));
            mix(&e.AlignedByteOffset, sizeof(e.AlignedByteOffset));
            mix(&e.InputSlotClass, sizeof(e.InputSlotClass));
            mix(&e.InstanceDataStepRate, sizeof(e.InstanceDataStepRate));
        }
    mix(&desc->PrimitiveTopologyType, sizeof(desc->PrimitiveTopologyType));
    return h;
}

bool DX12Runtime::LoadShaderReplacement(UINT64 hash, const char* stage, std::vector<BYTE>& bytes) const
{
    if (!hash || !stage) return false;
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%lsShaderFixes\\%016llx-%S_replace.bin", BasePath().c_str(), static_cast<unsigned long long>(hash), stage);
    if (!ReadFileBytes(path, bytes)) {
        swprintf_s(path, L"%lsShaderFixes\\%016llx-%S.bin", BasePath().c_str(), static_cast<unsigned long long>(hash), stage);
        if (!ReadFileBytes(path, bytes)) return false;
    }
    return !bytes.empty();
}

void DX12Runtime::Initialize(ID3D12Device* device)
{
    if (!device) return;
    bool expected = false;
    if (!m_initialized.compare_exchange_strong(expected, true)) return;
    m_device = device;
    m_device->AddRef();
    LoadConfig();
    DX12InstallRuntimeEntryHooks();
    LogInfo("*** 3DMigoto DX12 runtime initialized. ***\n");
}

void DX12Runtime::Shutdown()
{
    if (!m_initialized.exchange(false)) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    ReleaseCaptures();
    ReleaseFileResources();
    for (auto& p : m_pipelines) if (p.first) p.first->Release();
    for (auto& l : m_lists) if (l.second.pso) l.second.pso->Release();
    m_pipelines.clear();
    m_lists.clear();
    m_resources.clear();
    if (m_device) { m_device->Release(); m_device = nullptr; }
    if (m_log.is_open()) m_log.close();
}

void DX12Runtime::ReleaseFileResources()
{
    for (auto& kv : m_fileResources) if (kv.second.gpu) kv.second.gpu->Release();
    m_fileResources.clear();
}

void DX12Runtime::ReleaseCaptures()
{
    for (auto& c : m_captures) {
        if (c.readback) c.readback->Release();
        if (c.source) c.source->Release();
    }
    m_captures.clear();
}

void DX12Runtime::LoadConfig()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_captureVB = true;
    m_captureIB = true;
    m_captureOnce = false;
    m_autoFrameAnalysis = false;
    m_endAnalysisOnPresent = true;
    m_skipShaders.clear();
    m_textureOverrides.clear();
    ReleaseFileResources();
    LoadResourcesAndOverrides(IniPath());
    m_autoFrameAnalysis = IsTrueW(IniValue(IniPath(), L"Rendering", L"analyse_frame"));
    if (m_autoFrameAnalysis) {
        // Do not create the directory during DLL initialization. It is created on the first draw.
        m_frameAnalysis = true;
    }
    std::wstring once = IniValue(IniPath(), L"FrameAnalysis", L"once");
    if (!once.empty()) m_captureOnce = IsTrueW(once);
}

void DX12Runtime::LoadResourcesAndOverrides(const std::wstring& iniPath)
{
    std::vector<std::wstring> sections = IniSections(iniPath);
    for (const std::wstring& section : sections) {
        std::wstring lower = LowerW(section);
        if (PrefixCI(section, L"ShaderOverride")) {
            std::wstring hashText = IniValue(iniPath, section, L"hash");
            if (!hashText.empty() && IsTrueW(IniValue(iniPath, section, L"handling")))
                m_skipShaders[ParseHex64(hashText)] = true;
            else if (!hashText.empty() && LowerW(IniValue(iniPath, section, L"handling")) == L"skip")
                m_skipShaders[ParseHex64(hashText)] = true;
            continue;
        }
        if (PrefixCI(section, L"TextureOverride")) continue;
        if (!IsBufferResourceSection(iniPath, section)) continue;

        ResourceFile r;
        r.section = section;
        r.stride = 0;
        ParseUInt(IniValue(iniPath, section, L"stride"), &r.stride);
        r.format = ParseDXGIFormatW(IniValue(iniPath, section, L"format"));
        std::wstring filename = IniValue(iniPath, section, L"filename");
        if (filename.empty()) filename = IniValue(iniPath, section, L"file");
        if (!filename.empty()) r.path = ResolvePath(filename);
        if (r.path.empty()) {
            std::wstring data = IniValue(iniPath, section, L"data");
            if (!data.empty()) {
                // Support the common type=Buffer data="..." form used by generated INIs.
                size_t q1 = data.find(L'"'), q2 = data.find_last_of(L'"');
                if (q1 != std::wstring::npos && q2 > q1) data = data.substr(q1 + 1, q2 - q1 - 1);
                if (data.size() <= 8192) {
                    std::string utf8;
                    int n = WideCharToMultiByte(CP_UTF8, 0, data.c_str(), static_cast<int>(data.size()), nullptr, 0, nullptr, nullptr);
                    utf8.resize(n);
                    WideCharToMultiByte(CP_UTF8, 0, data.c_str(), static_cast<int>(data.size()), &utf8[0], n, nullptr, nullptr);
                    r.bytes.assign(utf8.begin(), utf8.end());
                }
            }
        }
        m_fileResources[LowerW(section)] = r;
    }
    LoadShaderSkipRules(iniPath);
    LoadTextureOverrides(iniPath);
}

void DX12Runtime::LoadShaderSkipRules(const std::wstring& iniPath)
{
    for (const std::wstring& section : IniSections(iniPath)) {
        if (!PrefixCI(section, L"ShaderOverride")) continue;
        std::wstring h = IniValue(iniPath, section, L"hash");
        std::wstring handling = LowerW(TrimW(IniValue(iniPath, section, L"handling")));
        if (!h.empty() && handling.find(L"skip") != std::wstring::npos)
            m_skipShaders[ParseHex64(h)] = true;
    }
}

void DX12Runtime::LoadTextureOverrides(const std::wstring& iniPath)
{
    for (const std::wstring& section : IniSections(iniPath)) {
        if (!PrefixCI(section, L"TextureOverride")) continue;
        std::wstring hashText = IniValue(iniPath, section, L"hash");
        if (hashText.empty()) continue;
        TextureOverride o;
        o.section = section;
        o.hash = ParseHex64(hashText);
        std::wstring handling = LowerW(TrimW(IniValue(iniPath, section, L"handling")));
        o.skip = handling.find(L"skip") != std::wstring::npos;
        for (UINT slot = 0; slot < 32; ++slot) {
            wchar_t key[16]; swprintf_s(key, L"vb%u", slot);
            std::wstring v = IniValue(iniPath, section, key);
            if (!v.empty()) { o.vb[slot] = v; o.hasVB[slot] = true; }
        }
        o.ib = IniValue(iniPath, section, L"ib");
        o.hasIB = !o.ib.empty();
        UINT v;
        if (ParseUInt(IniValue(iniPath, section, L"match_first_index"), &v)) { o.hasMatchFirstIndex = true; o.matchFirstIndex = v; }
        if (ParseUInt(IniValue(iniPath, section, L"match_first_vertex"), &v)) { o.hasMatchFirstVertex = true; o.matchFirstVertex = v; }
        if (ParseUInt(IniValue(iniPath, section, L"match_vertex_count"), &v)) { o.hasMatchVertexCount = true; o.matchVertexCount = v; }
        if (ParseUInt(IniValue(iniPath, section, L"match_first_instance"), &v)) { o.hasMatchInstance = true; o.matchFirstInstance = v; }
        if (ParseUInt(IniValue(iniPath, section, L"match_instance_count"), &v)) { o.hasMatchInstanceCount = true; o.matchInstanceCount = v; }

        std::wstring draw = IniValue(iniPath, section, L"draw");
        if (!draw.empty()) {
            unsigned a = 0, b = 0;
            if (swscanf_s(draw.c_str(), L"%u,%u", &a, &b) == 2) { o.hasDraw = true; o.drawCount = a; o.drawFirst = b; }
        }
        std::wstring drawIndexed = IniValue(iniPath, section, L"drawindexed");
        if (!drawIndexed.empty()) {
            unsigned a = 0, b = 0; int c = 0;
            if (swscanf_s(drawIndexed.c_str(), L"%u,%u,%d", &a, &b, &c) == 3) { o.hasDrawIndexed = true; o.drawIndexedCount = a; o.drawIndexedFirst = b; o.drawIndexedBase = c; }
        }
        m_textureOverrides.push_back(o);
    }
}

void DX12Runtime::OnGraphicsPipelineCreated(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* original, ID3D12PipelineState* pso, UINT64 originalHash)
{
    if (!original || !pso) return;
    PipelineInfo info;
    info.psoHash = originalHash;
    auto shaderHash = [this](const D3D12_SHADER_BYTECODE& b) -> UINT64 {
        return b.pShaderBytecode && b.BytecodeLength ? HashShader(b.pShaderBytecode, b.BytecodeLength) : 0;
    };
    info.vsHash = shaderHash(original->VS);
    info.hsHash = shaderHash(original->HS);
    info.dsHash = shaderHash(original->DS);
    info.gsHash = shaderHash(original->GS);
    info.psHash = shaderHash(original->PS);
    info.topologyType = original->PrimitiveTopologyType;
    if (original->InputLayout.pInputElementDescs) {
        for (UINT i = 0; i < original->InputLayout.NumElements; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& e = original->InputLayout.pInputElementDescs[i];
            InputElement x;
            x.semantic = e.SemanticName ? e.SemanticName : "";
            x.semanticIndex = e.SemanticIndex;
            x.format = e.Format;
            x.inputSlot = e.InputSlot;
            x.offset = e.AlignedByteOffset;
            x.slotClass = e.InputSlotClass;
            x.stepRate = e.InstanceDataStepRate;
            info.input.push_back(x);
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    pso->AddRef();
    m_pipelines[pso] = info;
}

void DX12Runtime::OnComputePipelineCreated(const D3D12_COMPUTE_PIPELINE_STATE_DESC* original, ID3D12PipelineState* pso, UINT64 originalHash)
{
    (void)original; (void)pso; (void)originalHash;
}

void DX12Runtime::RegisterResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState)
{
    if (!resource) return;
    ResourceInfo info;
    info.desc = resource->GetDesc();
    info.state = initialState;
    info.stateKnown = true;
    info.isBuffer = info.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    info.gpuVA = resource->GetGPUVirtualAddress();
    info.size = info.desc.Width;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_resources[resource] = info;
}

void DX12Runtime::UnregisterResource(ID3D12Resource* resource)
{
    if (!resource) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_resources.erase(resource);
}

void DX12Runtime::TrackSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
    if (!list) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    DrawState& state = m_lists[list];
    if (state.pso) state.pso->Release();
    state.pso = pso;
    if (pso) pso->AddRef();
}

void DX12Runtime::ResetCommandList(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
    if (!list) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    DrawState& state = m_lists[list];
    if (state.pso) state.pso->Release();
    state = DrawState{};
    state.pso = pso;
    if (pso) pso->AddRef();
}

void DX12Runtime::OnCommandListClose(ID3D12GraphicsCommandList* list)
{
    (void)list;
}

void DX12Runtime::TrackIAIndex(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view)
{
    if (!list) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    DrawState& s = m_lists[list];
    if (view) { s.ib = *view; s.ibSet = view->BufferLocation != 0; }
    else { s.ib = {}; s.ibSet = false; }
}

void DX12Runtime::TrackIAVertex(ID3D12GraphicsCommandList* list, UINT startSlot, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views)
{
    if (!list || startSlot >= 32) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    DrawState& s = m_lists[list];
    for (UINT i = 0; i < count && startSlot + i < 32; ++i) {
        if (views) { s.vb[startSlot + i] = views[i]; s.vbSet[startSlot + i] = views[i].BufferLocation != 0; }
        else { s.vb[startSlot + i] = {}; s.vbSet[startSlot + i] = false; }
    }
}

void DX12Runtime::TrackIATopology(ID3D12GraphicsCommandList* list, D3D12_PRIMITIVE_TOPOLOGY topology)
{
    if (!list) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lists[list].topology = topology;
}

void DX12Runtime::OnResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    if (!barriers) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (UINT i = 0; i < count; ++i) {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource) {
            auto it = m_resources.find(b.Transition.pResource);
            if (it != m_resources.end()) {
                it->second.state = b.Transition.StateAfter;
                it->second.stateKnown = true;
            }
        }
    }
}

void DX12Runtime::OnEnhancedBarrier(UINT count, const D3D12_BARRIER_GROUP* groups)
{
    if (!groups) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (UINT i = 0; i < count; ++i) {
        const D3D12_BARRIER_GROUP& g = groups[i];
        if (g.Type != D3D12_BARRIER_TYPE_BUFFER || !g.pBufferBarriers) continue;
        for (UINT j = 0; j < g.NumBarriers; ++j) {
            const D3D12_BUFFER_BARRIER& b = g.pBufferBarriers[j];
            auto it = m_resources.find(b.pResource);
            if (it == m_resources.end()) continue;
            // Buffers have no layout in enhanced barriers. Translate common access masks
            // to the legacy states needed by our readback insertion.
            if (b.AccessAfter & D3D12_BARRIER_ACCESS_COPY_SOURCE) it->second.state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            else if (b.AccessAfter & D3D12_BARRIER_ACCESS_COPY_DEST) it->second.state = D3D12_RESOURCE_STATE_COPY_DEST;
            else if (b.AccessAfter & D3D12_BARRIER_ACCESS_INDEX_BUFFER) it->second.state = D3D12_RESOURCE_STATE_INDEX_BUFFER;
            else if (b.AccessAfter & D3D12_BARRIER_ACCESS_VERTEX_BUFFER) it->second.state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
            else if (b.AccessAfter & D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) it->second.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            else it->second.state = D3D12_RESOURCE_STATE_COMMON;
            it->second.stateKnown = true;
        }
    }
}

ID3D12Resource* DX12Runtime::FindResourceByVA(D3D12_GPU_VIRTUAL_ADDRESS address, UINT64* offset) const
{
    for (const auto& kv : m_resources) {
        ID3D12Resource* r = kv.first;
        const ResourceInfo& info = kv.second;
        if (!info.isBuffer || !info.gpuVA || address < info.gpuVA || address >= info.gpuVA + info.size) continue;
        if (offset) *offset = address - info.gpuVA;
        return r;
    }
    return nullptr;
}

DX12Runtime::ResourceFile* DX12Runtime::FindFileResource(const std::wstring& name)
{
    std::wstring key = LowerW(TrimW(name));
    auto it = m_fileResources.find(key);
    if (it == m_fileResources.end()) return nullptr;
    return &it->second;
}

bool DX12Runtime::EnsureResourceLoaded(const std::wstring& name)
{
    ResourceFile* r = FindFileResource(name);
    if (!r || r->gpu || !m_device) return r && r->gpu;
    if (r->bytes.empty() && !r->path.empty() && !ReadFileBytes(r->path, r->bytes)) return false;
    if (r->bytes.empty()) return false;
    if ((!r->stride || r->format == DXGI_FORMAT_UNKNOWN) && !r->path.empty()) {
        std::wstring fmtPath = ReplaceExtension(r->path, L".fmt");
        std::ifstream fmt(fmtPath.c_str(), std::ios::in | std::ios::binary);
        if (fmt.is_open()) {
            std::string line;
            while (std::getline(fmt, line)) {
                size_t colon = line.find(':');
                if (colon == std::string::npos) break;
                std::string key = line.substr(0, colon), value = line.substr(colon + 1);
                while (!key.empty() && isspace((unsigned char)key.back())) key.pop_back();
                while (!value.empty() && isspace((unsigned char)value.front())) value.erase(value.begin());
                if (key == "stride" && !r->stride) r->stride = static_cast<UINT>(strtoul(value.c_str(), nullptr, 0));
                if (key == "format" && r->format == DXGI_FORMAT_UNKNOWN) {
                    wchar_t w[128]{}; MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, w, ARRAYSIZE(w));
                    r->format = DX12ParseFormat(w);
                }
            }
        }
    }
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = r->bytes.size();
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_dx12Internal = true;
    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&r->gpu));
    g_dx12Internal = false;
    if (FAILED(hr)) { r->gpu = nullptr; return false; }
    D3D12_RANGE readRange{0, 0};
    void* mapped = nullptr;
    if (FAILED(r->gpu->Map(0, &readRange, &mapped))) { r->gpu->Release(); r->gpu = nullptr; return false; }
    memcpy(mapped, r->bytes.data(), r->bytes.size());
    r->gpu->Unmap(0, nullptr);
    r->gpuVA = r->gpu->GetGPUVirtualAddress();
    r->size = r->bytes.size();
    return true;
}

const DX12Runtime::TextureOverride* DX12Runtime::FindMatchingOverride(const PipelineInfo& pipeline, bool indexed,
                                                                        UINT first, UINT count, UINT firstInstance,
                                                                        UINT instanceCount) const
{
    for (const TextureOverride& o : m_textureOverrides) {
        if (!PipelineMatchesHash(pipeline, o.hash)) continue;
        if (indexed && o.hasMatchFirstIndex && o.matchFirstIndex != first) continue;
        if (!indexed && o.hasMatchFirstVertex && o.matchFirstVertex != first) continue;
        if (indexed && o.hasMatchVertexCount && o.matchVertexCount != count) continue;
        if (!indexed && o.hasMatchVertexCount && o.matchVertexCount != count) continue;
        if (o.hasMatchInstance && o.matchFirstInstance != firstInstance) continue;
        if (o.hasMatchInstanceCount && o.matchInstanceCount != instanceCount) continue;
        return &o;
    }
    return nullptr;
}

bool DX12Runtime::PipelineMatchesHash(const PipelineInfo& p, UINT64 hash) const
{
    return hash && (p.vsHash == hash || p.hsHash == hash || p.dsHash == hash || p.gsHash == hash || p.psHash == hash);
}

void DX12Runtime::ApplyVertexOverrides(ID3D12GraphicsCommandList* list, UINT startSlot, UINT count, D3D12_VERTEX_BUFFER_VIEW* views)
{
    if (!list || !views || startSlot >= 32) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto lit = m_lists.find(list);
    if (lit == m_lists.end() || !lit->second.pso) return;
    auto pit = m_pipelines.find(lit->second.pso);
    if (pit == m_pipelines.end()) return;
    for (const TextureOverride& o : m_textureOverrides) {
        if (!PipelineMatchesHash(pit->second, o.hash)) continue;
        for (UINT i = 0; i < count && startSlot + i < 32; ++i) {
            UINT slot = startSlot + i;
            if (!o.hasVB[slot]) continue;
            if (!EnsureResourceLoaded(o.vb[slot])) continue;
            ResourceFile* r = FindFileResource(o.vb[slot]);
            if (!r || !r->gpu) continue;
            views[i].BufferLocation = r->gpuVA;
            views[i].SizeInBytes = static_cast<UINT>(std::min<UINT64>(r->size, 0xffffffffULL));
            if (r->stride) views[i].StrideInBytes = r->stride;
        }
    }
}

void DX12Runtime::ApplyIndexOverride(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view)
{
    (void)list; (void)view;
}

bool DX12Runtime::ApplyIndexOverrideForDraw(ID3D12GraphicsCommandList* list, UINT firstIndex, UINT indexCount,
                                             INT baseVertex, UINT firstInstance, UINT instanceCount, D3D12_INDEX_BUFFER_VIEW* outView)
{
    if (!list || !outView) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto lit = m_lists.find(list);
    if (lit == m_lists.end() || !lit->second.pso || !lit->second.ibSet) return false;
    auto pit = m_pipelines.find(lit->second.pso);
    if (pit == m_pipelines.end()) return false;
    for (const TextureOverride& o : m_textureOverrides) {
        if (!PipelineMatchesHash(pit->second, o.hash)) continue;
        if (o.hasMatchFirstIndex && o.matchFirstIndex != firstIndex) continue;
        if (o.hasMatchInstance && o.matchFirstInstance != firstInstance) continue;
        if (o.hasMatchInstanceCount && o.matchInstanceCount != instanceCount) continue;
        if (!o.hasIB) continue;
        if (!EnsureResourceLoaded(o.ib)) continue;
        ResourceFile* r = FindFileResource(o.ib);
        if (!r || !r->gpu) continue;
        D3D12_INDEX_BUFFER_VIEW v = lit->second.ib;
        v.BufferLocation = r->gpuVA;
        v.SizeInBytes = static_cast<UINT>(std::min<UINT64>(r->size, 0xffffffffULL));
        if (r->format != DXGI_FORMAT_UNKNOWN) v.Format = r->format;
        *outView = v;
        (void)baseVertex;
        return true;
    }
    return false;
}

bool DX12Runtime::ShouldSkipDraw(ID3D12GraphicsCommandList* list, bool indexed, UINT first, UINT count,
                                 UINT firstInstance, UINT instanceCount) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto lit = m_lists.find(list);
    if (lit == m_lists.end() || !lit->second.pso) return false;
    auto pit = m_pipelines.find(lit->second.pso);
    if (pit == m_pipelines.end()) return false;
    const PipelineInfo& p = pit->second;
    if ((p.vsHash && m_skipShaders.find(p.vsHash) != m_skipShaders.end()) ||
        (p.hsHash && m_skipShaders.find(p.hsHash) != m_skipShaders.end()) ||
        (p.dsHash && m_skipShaders.find(p.dsHash) != m_skipShaders.end()) ||
        (p.gsHash && m_skipShaders.find(p.gsHash) != m_skipShaders.end()) ||
        (p.psHash && m_skipShaders.find(p.psHash) != m_skipShaders.end())) return true;
    bool customDraw = false;
    bool skip = false;
    for (const TextureOverride& o : m_textureOverrides) {
        if (!PipelineMatchesHash(p, o.hash)) continue;
        if (indexed && o.hasMatchFirstIndex && o.matchFirstIndex != first) continue;
        if (!indexed && o.hasMatchFirstVertex && o.matchFirstVertex != first) continue;
        if (o.hasMatchInstance && o.matchFirstInstance != firstInstance) continue;
        if (o.hasMatchInstanceCount && o.matchInstanceCount != instanceCount) continue;
        customDraw |= indexed ? o.hasDrawIndexed : o.hasDraw;
        if (o.skip) skip = true;
    }
    return skip && !customDraw;
}

bool DX12Runtime::GetDrawOverride(ID3D12GraphicsCommandList* list, bool indexed, UINT* count, UINT* first, INT* baseVertex) const
{
    if (!list || !count || !first || !baseVertex) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto lit = m_lists.find(list);
    if (lit == m_lists.end() || !lit->second.pso) return false;
    auto pit = m_pipelines.find(lit->second.pso);
    if (pit == m_pipelines.end()) return false;
    for (const TextureOverride& o : m_textureOverrides) {
        if (!PipelineMatchesHash(pit->second, o.hash)) continue;
        if (indexed && o.hasDrawIndexed) { *count = o.drawIndexedCount; *first = o.drawIndexedFirst; *baseVertex = o.drawIndexedBase; return true; }
        if (!indexed && o.hasDraw) { *count = o.drawCount; *first = o.drawFirst; *baseVertex = 0; return true; }
    }
    return false;
}

std::wstring DX12Runtime::AnalysisPath()
{
    if (!m_analysisDir.empty()) return m_analysisDir;
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t path[MAX_PATH]{};
    swprintf_s(path, L"%lsFrameAnalysis-%04u-%02u-%02u-%02u%02u%02u", BasePath().c_str(),
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    if (!CreateDirectoryW(path, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    m_analysisDir = path;
    return m_analysisDir;
}

bool DX12Runtime::ScheduleReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                                   UINT64 sourceOffset, UINT64 size, ID3D12Resource** readback)
{
    if (!list || !source || !size || !readback || !m_device) return false;
    auto it = m_resources.find(source);
    if (it == m_resources.end() || !it->second.stateKnown) return false;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_dx12Internal = true;
    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(readback));
    g_dx12Internal = false;
    if (FAILED(hr)) return false;

    D3D12_RESOURCE_STATES old = it->second.state;
    if (old != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = source;
        b.Transition.StateBefore = old;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12Internal = true;
        list->ResourceBarrier(1, &b);
        g_dx12Internal = false;
    }
    list->CopyBufferRegion(*readback, 0, source, sourceOffset, size);
    if (old != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = source;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = old;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12Internal = true;
        list->ResourceBarrier(1, &b);
        g_dx12Internal = false;
    }
    return true;
}

bool DX12Runtime::CaptureVB(ID3D12GraphicsCommandList* list, const D3D12_VERTEX_BUFFER_VIEW& view,
                            UINT slot, const PipelineInfo& pipeline, UINT draw, UINT first, UINT count,
                            UINT firstInstance, UINT instanceCount)
{
    UINT64 offset = 0;
    ID3D12Resource* source = FindResourceByVA(view.BufferLocation, &offset);
    if (!source) return false;
    UINT64 size = source->GetDesc().Width;
    ID3D12Resource* readback = nullptr;
    if (!ScheduleReadback(list, source, 0, size, &readback)) return false;
    Capture c;
    c.source = source; source->AddRef();
    c.readback = readback;
    c.sourceOffset = offset;
    c.size = size;
    c.gpuAddress = view.BufferLocation;
    c.slot = slot;
    c.stride = view.StrideInBytes;
    c.draw = draw;
    c.first = first;
    c.count = count;
    c.baseVertex = static_cast<INT>(first);
    c.firstInstance = firstInstance;
    c.instanceCount = instanceCount;
    c.pipeline = pipeline;
    c.index = false;
    m_captures.push_back(c);
    return true;
}

bool DX12Runtime::CaptureIB(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW& view,
                            const PipelineInfo& pipeline, UINT draw, UINT first, UINT count,
                            UINT firstInstance, UINT instanceCount, INT baseVertex)
{
    UINT64 offset = 0;
    ID3D12Resource* source = FindResourceByVA(view.BufferLocation, &offset);
    if (!source) return false;
    UINT64 size = source->GetDesc().Width;
    ID3D12Resource* readback = nullptr;
    if (!ScheduleReadback(list, source, 0, size, &readback)) return false;
    Capture c;
    c.source = source; source->AddRef();
    c.readback = readback;
    c.sourceOffset = offset;
    c.size = size;
    c.gpuAddress = view.BufferLocation;
    c.format = view.Format;
    c.draw = draw;
    c.first = first;
    c.count = count;
    c.firstInstance = firstInstance;
    c.instanceCount = instanceCount;
    c.baseVertex = baseVertex;
    c.index = true;
    c.stride = view.Format == DXGI_FORMAT_R32_UINT ? 4 : 2;
    c.pipeline = pipeline;
    m_captures.push_back(c);
    return true;
}

void DX12Runtime::WriteFrameLogHeader()
{
    if (!m_log.is_open()) {
        std::wstring path = AnalysisPath();
        if (path.empty()) return;
        std::wstring logPath = path + L"\\log.txt";
        m_log.open(logPath.c_str(), std::ios::out | std::ios::binary);
        if (m_log.is_open()) m_log << "analyse_options: 00000000\n";
    }
}

std::string DX12Runtime::FormatText(const BYTE* data, DXGI_FORMAT format) const
{
    UINT n = FormatSize(format);
    if (!data || !n) return "";
    std::ostringstream out;
    // Keep the 3DMigoto text representation useful for XXMITools: scalar numeric formats are
    // printed as numbers; packed/compound formats are represented as raw bytes when no safe
    // scalar interpretation exists.
    if (format == DXGI_FORMAT_R32_FLOAT) { float v; memcpy(&v, data, 4); out << std::setprecision(9) << v; return out.str(); }
    if (format == DXGI_FORMAT_R32_UINT) { UINT v; memcpy(&v, data, 4); out << v; return out.str(); }
    if (format == DXGI_FORMAT_R32_SINT) { INT v; memcpy(&v, data, 4); out << v; return out.str(); }
    if (format == DXGI_FORMAT_R16_UINT) { UINT16 v; memcpy(&v, data, 2); out << v; return out.str(); }
    if (format == DXGI_FORMAT_R16_SINT) { INT16 v; memcpy(&v, data, 2); out << v; return out.str(); }
    out << "0x";
    for (UINT i = 0; i < n; ++i) out << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[i]);
    return out.str();
}

void DX12Runtime::WriteCapture(const Capture& c, const BYTE* data, UINT64 dataSize)
{
    if (!data || !dataSize) return;
    std::wstring dir = AnalysisPath();
    if (dir.empty()) return;
    UINT32 hash = CRC32C(data, static_cast<size_t>(dataSize));
    std::ostringstream stem;
    stem << std::setw(6) << std::setfill('0') << c.draw << "-";
    if (c.index) stem << "ib";
    else stem << "vb" << c.slot;
    stem << "=" << Hex32(hash);
    if (c.pipeline.vsHash) stem << "-vs=" << Hex64(c.pipeline.vsHash);
    if (c.pipeline.hsHash) stem << "-hs=" << Hex64(c.pipeline.hsHash);
    if (c.pipeline.dsHash) stem << "-ds=" << Hex64(c.pipeline.dsHash);
    if (c.pipeline.gsHash) stem << "-gs=" << Hex64(c.pipeline.gsHash);
    if (c.pipeline.psHash) stem << "-ps=" << Hex64(c.pipeline.psHash);
    std::string s = stem.str();
    std::wstring ws(s.begin(), s.end());

    std::wstring bufPath = dir + L"\\" + ws + L".buf";
    HANDLE file = CreateFileW(bufPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, data, static_cast<DWORD>(dataSize), &written, nullptr);
        CloseHandle(file);
    }

    std::wstring txtBase = ws;
    if (!c.index) {
        std::ostringstream suffix;
        suffix << "-vb" << c.slot << "-topology=" << TopologyName(c.pipeline.topology);
        if (c.sourceOffset) suffix << "-offset=" << c.sourceOffset;
        if (c.stride) suffix << "-stride=" << c.stride;
        if (c.first) suffix << "-first=" << c.first;
        if (c.count) suffix << "-count=" << c.count;
        if (c.firstInstance) suffix << "-first_inst=" << c.firstInstance;
        if (c.instanceCount) suffix << "-inst_count=" << c.instanceCount;
        std::string suffixText = suffix.str();
        txtBase += std::wstring(suffixText.begin(), suffixText.end());
    } else {
        std::ostringstream suffix;
        suffix << "-ib-format=" << FormatName(c.format) << "-topology=" << TopologyName(c.pipeline.topology);
        if (c.sourceOffset) suffix << "-offset=" << c.sourceOffset;
        if (c.first) suffix << "-first=" << c.first;
        if (c.count) suffix << "-count=" << c.count;
        std::string suffixText = suffix.str();
        txtBase += std::wstring(suffixText.begin(), suffixText.end());
    }
    std::wstring txtPath = dir + L"\\" + txtBase + L".txt";
    

    // XXMITools can consume a .fmt sidecar. Generate it from the same metadata so the binary
    // dump is directly importable without depending on the legacy FA text parser.
    std::wstring fmtPath = dir + L"\\" + ws + L".fmt";
    std::ofstream fmt(fmtPath.c_str(), std::ios::out | std::ios::binary);
    if (!fmt.is_open()) return;
    if (c.index) {
        fmt << "byte offset: " << c.sourceOffset << "\n";
        if (c.first || c.count) { fmt << "first index: " << c.first << "\n"; fmt << "index count: " << c.count << "\n"; }
        fmt << "topology: " << TopologyName(c.pipeline.topology) << "\n";
        fmt << "format: " << FormatName(c.format) << "\n";
    } else {
        fmt << "byte offset: " << c.sourceOffset << "\n";
        fmt << "stride: " << c.stride << "\n";
        fmt << "first vertex: " << c.first << "\n";
        fmt << "vertex count: " << c.count << "\n";
        if (c.firstInstance || c.instanceCount) { fmt << "first instance: " << c.firstInstance << "\n"; fmt << "instance count: " << c.instanceCount << "\n"; }
        fmt << "topology: " << TopologyName(c.pipeline.topology) << "\n";
        UINT fmtElement = 0;
        UINT slotCursor[32]{};
        for (size_t i = 0; i < c.pipeline.input.size(); ++i) {
            const InputElement& e = c.pipeline.input[i];
            if (e.inputSlot != c.slot) continue;
            UINT offset = e.offset;
            if (offset == D3D12_APPEND_ALIGNED_ELEMENT) offset = slotCursor[e.inputSlot];
            slotCursor[e.inputSlot] = offset + FormatSize(e.format);
            fmt << "element[" << fmtElement++ << "]: \n";
            fmt << " SemanticName: " << e.semantic << "\n";
            fmt << " SemanticIndex: " << e.semanticIndex << "\n";
            fmt << " Format: " << FormatName(e.format) << "\n";
            fmt << " InputSlot: " << e.inputSlot << "\n";
            fmt << " AlignedByteOffset: " << offset << "\n";
            fmt << " InputSlotClass: " << (e.slotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? "per-instance" : "per-vertex") << "\n";
            fmt << " InstanceDataStepRate: " << e.stepRate << "\n";
        }
    }
}

void DX12Runtime::WriteFrameLogForDraw(const DrawState& state, const PipelineInfo& pipeline, UINT draw,
                                       bool indexed, UINT first, UINT count, INT baseVertex,
                                       UINT firstInstance, UINT instanceCount,
                                       const std::vector<std::pair<UINT, UINT32>>& vbHashes,
                                       UINT32 ibHash)
{
    WriteFrameLogHeader();
    if (!m_log.is_open()) return;
    m_log << std::setw(6) << std::setfill('0') << draw << " ";
    if (indexed) m_log << "DrawIndexedInstanced(IndexCountPerInstance:" << count << ", InstanceCount:" << instanceCount
                       << ", StartIndexLocation:" << first << ", BaseVertexLocation:" << baseVertex
                       << ", StartInstanceLocation:" << firstInstance << ")\n";
    else m_log << "DrawInstanced(VertexCountPerInstance:" << count << ", InstanceCount:" << instanceCount
               << ", StartVertexLocation:" << first << ", StartInstanceLocation:" << firstInstance << ")\n";
    if (indexed && state.ibSet) {
        m_log << "      IASetIndexBuffer(pIndexBuffer:0x" << std::hex << state.ib.BufferLocation << std::dec
              << ", Format:" << state.ib.Format << ", Offset:" << 0 << ") resource=0x" << std::hex
              << state.ib.BufferLocation << std::dec << " hash=" << Hex32(ibHash) << "\n";
    }
    for (const auto& h : vbHashes) {
        m_log << "      IASetVertexBuffers(StartSlot:" << h.first << ", NumBuffers:1, ppVertexBuffers:... )\n";
        m_log << "        " << h.first << ": resource=0x" << std::hex;
        for (UINT i = 0; i < 32; ++i) if (state.vbSet[i] && i == h.first) { m_log << state.vb[i].BufferLocation; break; }
        m_log << std::dec << " hash=" << Hex32(h.second) << "\n";
    }
}

void DX12Runtime::OnDrawIndexed(UINT indexCount, UINT instanceCount, UINT firstIndex, INT baseVertex,
                                UINT firstInstance, ID3D12GraphicsCommandList* list)
{
    SHORT f8 = GetAsyncKeyState(VK_F8);
    if ((f8 & 0x8000) && !m_f8Latch) OnFrameAnalysisKey();
    m_f8Latch = (f8 & 0x8000) != 0;
    if (!m_frameAnalysis) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto lit = m_lists.find(list);
    if (lit == m_lists.end() || !lit->second.pso) return;
    auto pit = m_pipelines.find(lit->second.pso);
    if (pit == m_pipelines.end()) return;
    DrawState state = lit->second;
    PipelineInfo pipeline = pit->second;
    pipeline.topology = state.topology;
    ++m_drawCall;
    if (m_captureVB) for (UINT slot = 0; slot < 32; ++slot) if (state.vbSet[slot])
        CaptureVB(list, state.vb[slot], slot, pipeline, m_drawCall, baseVertex < 0 ? 0 : static_cast<UINT>(baseVertex), 0, firstInstance, instanceCount);
    if (m_captureIB && state.ibSet) CaptureIB(list, state.ib, pipeline, m_drawCall, firstIndex, indexCount, firstInstance, instanceCount, baseVertex);
}

void DX12Runtime::OnDraw(UINT vertexCount, UINT instanceCount, UINT firstVertex, UINT firstInstance, ID3D12GraphicsCommandList* list)
{
    SHORT f8 = GetAsyncKeyState(VK_F8);
    if ((f8 & 0x8000) && !m_f8Latch) OnFrameAnalysisKey();
    m_f8Latch = (f8 & 0x8000) != 0;
    if (!m_frameAnalysis) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto lit = m_lists.find(list);
    if (lit == m_lists.end() || !lit->second.pso) return;
    auto pit = m_pipelines.find(lit->second.pso);
    if (pit == m_pipelines.end()) return;
    DrawState state = lit->second;
    PipelineInfo pipeline = pit->second;
    pipeline.topology = state.topology;
    ++m_drawCall;
    if (m_captureVB) for (UINT slot = 0; slot < 32; ++slot) if (state.vbSet[slot])
        CaptureVB(list, state.vb[slot], slot, pipeline, m_drawCall, firstVertex, vertexCount, firstInstance, instanceCount);
}

void DX12Runtime::WritePendingCaptures()
{
    if (m_captures.empty()) return;
    std::map<UINT, std::vector<Capture*>> byDraw;
    for (Capture& c : m_captures) byDraw[c.draw].push_back(&c);

    for (auto& group : byDraw) {
        struct Mapped { Capture* c; BYTE* p; };
        std::vector<Mapped> mapped;
        Capture* ib = nullptr;
        UINT maxVertex = 0;
        bool haveMaxVertex = false;

        for (Capture* c : group.second) {
            void* ptr = nullptr;
            if (FAILED(c->readback->Map(0, nullptr, &ptr))) continue;
            mapped.push_back({ c, static_cast<BYTE*>(ptr) });
            if (c->index && !ib) ib = c;
        }

        if (ib && ib->count) {
            UINT width = ib->format == DXGI_FORMAT_R32_UINT ? 4 : 2;
            UINT64 start = ib->sourceOffset / width + ib->first;
            UINT64 end = std::min<UINT64>(ib->size / width, start + ib->count);
            const BYTE* data = nullptr;
            for (const Mapped& m : mapped) if (m.c == ib) { data = m.p; break; }
            if (data) {
                for (UINT64 i = start; i < end; ++i) {
                    UINT value = 0;
                    if (width == 2) { UINT16 x; memcpy(&x, data + i * width, 2); value = x; }
                    else { UINT x; memcpy(&x, data + i * width, 4); value = x; }
                    INT signedValue = static_cast<INT>(value) + ib->baseVertex;
                    if (signedValue >= 0) {
                        maxVertex = std::max(maxVertex, static_cast<UINT>(signedValue));
                        haveMaxVertex = true;
                    }
                }
            }
        }

        std::vector<std::pair<UINT, UINT32>> vbHashes;
        UINT32 ibHash = 0;
        bool indexed = ib != nullptr;
        UINT first = 0, count = 0, firstInstance = 0, instanceCount = 0;
        INT baseVertex = 0;
        PipelineInfo pipeline{};

        for (Mapped& m : mapped) {
            Capture local = *m.c;
            if (!local.index && indexed && haveMaxVertex) {
                // Match 3DMigoto's indexed-draw behaviour: first vertex comes from
                // BaseVertexLocation and the dump count is max(index)+1.
                local.first = local.baseVertex;
                local.count = maxVertex + 1;
            }
            UINT32 h = CRC32C(m.p, static_cast<size_t>(local.size));
            WriteCapture(local, m.p, local.size);
            pipeline = local.pipeline;
            if (local.index) {
                ibHash = h;
                first = local.first;
                count = local.count;
                firstInstance = local.firstInstance;
                instanceCount = local.instanceCount;
                baseVertex = local.baseVertex;
            } else {
                vbHashes.push_back({ local.slot, h });
            }
        }

        if (!mapped.empty()) {
            WriteFrameLogHeader();
            if (m_log.is_open()) {
                UINT draw = group.first;
                m_log << std::setw(6) << std::setfill('0') << draw << " ";
                if (indexed) {
                    m_log << "DrawIndexedInstanced(IndexCountPerInstance:" << count
                          << ", InstanceCount:" << instanceCount
                          << ", StartIndexLocation:" << first
                          << ", BaseVertexLocation:" << baseVertex
                          << ", StartInstanceLocation:" << firstInstance << ")\n";
                } else {
                    m_log << "DrawInstanced(VertexCountPerInstance:" << count
                          << ", InstanceCount:" << instanceCount
                          << ", StartVertexLocation:" << first
                          << ", StartInstanceLocation:" << firstInstance << ")\n";
                }
                for (const Mapped& m : mapped) {
                    if (m.c->index) {
                        m_log << "      IASetIndexBuffer(StartSlot:0, NumBuffers:1)\n";
                        m_log << "        0: resource=0x" << std::hex << m.c->gpuAddress
                              << std::dec << " hash=" << Hex32(ibHash) << "\n";
                    } else {
                        UINT32 h = CRC32C(m.p, static_cast<size_t>(m.c->size));
                        m_log << "      IASetVertexBuffers(StartSlot:" << m.c->slot << ", NumBuffers:1)\n";
                        m_log << "        " << m.c->slot << ": resource=0x" << std::hex << m.c->gpuAddress
                              << std::dec << " hash=" << Hex32(h) << "\n";
                    }
                }
            }
        }

        for (const Mapped& m : mapped) m.c->readback->Unmap(0, nullptr);
    }
}

void DX12Runtime::OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    (void)count; (void)lists;
    if (!m_initialized || !m_frameAnalysis || m_captures.empty()) return;
    if (!queue) return;

    ID3D12Fence* fence = nullptr;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
    static std::atomic<UINT64> fenceValue{0};
    UINT64 value = ++fenceValue;
    if (FAILED(queue->Signal(fence, value))) { fence->Release(); return; }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event) {
        if (fence->GetCompletedValue() < value) fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }
    fence->Release();

    std::lock_guard<std::mutex> lock(m_mutex);
    WritePendingCaptures();
    ReleaseCaptures();
}

void DX12Runtime::OnPresent()
{
    // Present is the frame boundary. Frame Analysis is intentionally one-frame unless
    // the user starts another capture with F8.
    SHORT f8 = GetAsyncKeyState(VK_F8);
    bool pressed = (f8 & 0x8000) != 0;
    if (pressed && !m_f8Latch) {
        OnFrameAnalysisKey();
        m_f8Latch = pressed;
        return;
    }
    m_f8Latch = pressed;

    if (m_frameAnalysis && m_endAnalysisOnPresent)
        m_frameAnalysis = false;
}

void DX12Runtime::OnFrameAnalysisKey()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameAnalysis = !m_frameAnalysis;
    if (m_frameAnalysis) {
        m_drawCall = 0;
        m_analysisDir.clear();
        WriteFrameLogHeader();
    }
}

bool DX12Runtime::IsFrameAnalysisActive() const
{
    return m_frameAnalysis;
}

