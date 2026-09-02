#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <fstream>

class DX12Runtime {
public:
    static DX12Runtime& Instance();

    void Initialize(ID3D12Device* device);
    void Shutdown();

    UINT64 HashShader(const void* data, SIZE_T size) const;
    UINT64 HashPipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc) const;
    bool LoadShaderReplacement(UINT64 hash, const char* stage, std::vector<BYTE>& bytes) const;

    struct InputElement {
        std::string semantic;
        UINT semanticIndex = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT inputSlot = 0;
        UINT offset = 0;
        D3D12_INPUT_CLASSIFICATION slotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        UINT stepRate = 0;
    };

    struct PipelineInfo {
        UINT64 psoHash = 0;
        UINT64 vsHash = 0;
        UINT64 hsHash = 0;
        UINT64 dsHash = 0;
        UINT64 gsHash = 0;
        UINT64 psHash = 0;
        std::vector<InputElement> input;
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
        D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    };

    struct ResourceFile {
        std::wstring section;
        std::wstring path;
        UINT stride = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::vector<BYTE> bytes;
        ID3D12Resource* gpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
        UINT64 size = 0;
    };

    struct TextureOverride {
        std::wstring section;
        UINT64 hash = 0;
        std::wstring vb[32];
        bool hasVB[32]{};
        std::wstring ib;
        bool hasIB = false;
        bool skip = false;
        bool hasMatchFirstIndex = false;
        UINT matchFirstIndex = 0;
        bool hasMatchFirstVertex = false;
        UINT matchFirstVertex = 0;
        bool hasMatchVertexCount = false;
        UINT matchVertexCount = 0;
        bool hasMatchInstance = false;
        UINT matchFirstInstance = 0;
        bool hasMatchInstanceCount = false;
        UINT matchInstanceCount = 0;
        bool hasDraw = false;
        UINT drawCount = 0;
        UINT drawFirst = 0;
        bool hasDrawIndexed = false;
        UINT drawIndexedCount = 0;
        UINT drawIndexedFirst = 0;
        INT drawIndexedBase = 0;
    };

    void OnGraphicsPipelineCreated(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* original,
                                   ID3D12PipelineState* pso,
                                   UINT64 originalHash);
    void OnComputePipelineCreated(const D3D12_COMPUTE_PIPELINE_STATE_DESC* original,
                                  ID3D12PipelineState* pso,
                                  UINT64 originalHash);

    void RegisterResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState);
    void UnregisterResource(ID3D12Resource* resource);

    void TrackSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso);
    void ResetCommandList(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso);
    void OnCommandListClose(ID3D12GraphicsCommandList* list);
    void TrackIAIndex(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view);
    void TrackIAVertex(ID3D12GraphicsCommandList* list, UINT startSlot, UINT count,
                       const D3D12_VERTEX_BUFFER_VIEW* views);
    void TrackIATopology(ID3D12GraphicsCommandList* list, D3D12_PRIMITIVE_TOPOLOGY topology);
    void OnResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers);
    void OnEnhancedBarrier(UINT count, const D3D12_BARRIER_GROUP* groups);

    void ApplyVertexOverrides(ID3D12GraphicsCommandList* list, UINT startSlot, UINT count,
                              D3D12_VERTEX_BUFFER_VIEW* views);
    void ApplyIndexOverride(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view);
    bool ApplyIndexOverrideForDraw(ID3D12GraphicsCommandList* list, UINT firstIndex,
                                   UINT indexCount, INT baseVertex, UINT firstInstance,
                                   UINT instanceCount, D3D12_INDEX_BUFFER_VIEW* outView);
    bool ShouldSkipDraw(ID3D12GraphicsCommandList* list, bool indexed, UINT firstVertexOrIndex,
                        UINT count, UINT firstInstance, UINT instanceCount) const;
    bool GetDrawOverride(ID3D12GraphicsCommandList* list, bool indexed, UINT* count, UINT* first, INT* baseVertex) const;
    void OnDrawIndexed(UINT indexCount, UINT instanceCount, UINT firstIndex, INT baseVertex,
                       UINT firstInstance, ID3D12GraphicsCommandList* list);
    void OnDraw(UINT vertexCount, UINT instanceCount, UINT firstVertex, UINT firstInstance,
                ID3D12GraphicsCommandList* list);

    void OnExecute(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists);
    void OnPresent();

    void OnFrameAnalysisKey();
    bool IsFrameAnalysisActive() const;

private:
    DX12Runtime() = default;
    ~DX12Runtime() = default;
    DX12Runtime(const DX12Runtime&) = delete;
    DX12Runtime& operator=(const DX12Runtime&) = delete;

    struct ResourceInfo {
        D3D12_RESOURCE_DESC desc{};
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        bool stateKnown = false;
        bool isBuffer = false;
        D3D12_GPU_VIRTUAL_ADDRESS gpuVA = 0;
        UINT64 size = 0;
    };

    struct DrawState {
        ID3D12PipelineState* pso = nullptr;
        D3D12_VERTEX_BUFFER_VIEW vb[32]{};
        bool vbSet[32]{};
        D3D12_INDEX_BUFFER_VIEW ib{};
        bool ibSet = false;
        D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    };

    struct Capture {
        ID3D12Resource* source = nullptr;
        ID3D12Resource* readback = nullptr;
        UINT64 sourceOffset = 0;
        UINT64 size = 0;
        UINT64 gpuAddress = 0;
        UINT slot = 0;
        UINT stride = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT draw = 0;
        UINT first = 0;
        UINT count = 0;
        UINT firstInstance = 0;
        UINT instanceCount = 0;
        INT baseVertex = 0;
        bool index = false;
        PipelineInfo pipeline;
    };

    void LoadConfig();
    void LoadResourcesAndOverrides(const std::wstring& iniPath);
    void LoadShaderSkipRules(const std::wstring& iniPath);
    void LoadTextureOverrides(const std::wstring& iniPath);

    bool EnsureResourceLoaded(const std::wstring& name);
    ResourceFile* FindFileResource(const std::wstring& name);
    const TextureOverride* FindMatchingOverride(const PipelineInfo& pipeline, bool indexed,
                                                UINT first, UINT count, UINT firstInstance,
                                                UINT instanceCount) const;
    bool PipelineMatchesHash(const PipelineInfo& pipeline, UINT64 hash) const;

    ID3D12Resource* FindResourceByVA(D3D12_GPU_VIRTUAL_ADDRESS address, UINT64* offset) const;
    bool ScheduleReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                          UINT64 sourceOffset, UINT64 size, ID3D12Resource** readback);
    bool CaptureVB(ID3D12GraphicsCommandList* list, const D3D12_VERTEX_BUFFER_VIEW& view,
                   UINT slot, const PipelineInfo& pipeline, UINT draw, UINT first, UINT count,
                   UINT firstInstance, UINT instanceCount);
    bool CaptureIB(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW& view,
                   const PipelineInfo& pipeline, UINT draw, UINT first, UINT count,
                   UINT firstInstance, UINT instanceCount, INT baseVertex);
    void WritePendingCaptures();
    void WriteCapture(const Capture& capture, const BYTE* data, UINT64 dataSize);
    void WriteFrameLogHeader();
    void WriteFrameLogForDraw(const DrawState& state, const PipelineInfo& pipeline,
                              UINT draw, bool indexed, UINT first, UINT count, INT baseVertex,
                              UINT firstInstance, UINT instanceCount,
                              const std::vector<std::pair<UINT, UINT32>>& vbHashes,
                              UINT32 ibHash);

    std::wstring BasePath() const;
    std::wstring IniPath() const;
    std::wstring AnalysisPath();
    std::wstring ResolvePath(const std::wstring& p) const;
    std::string Hex32(UINT32 value) const;
    std::string Hex64(UINT64 value) const;
    UINT32 CRC32C(const BYTE* data, size_t size) const;
    UINT FormatSize(DXGI_FORMAT format) const;
    const char* FormatName(DXGI_FORMAT format) const;
    const char* TopologyName(D3D12_PRIMITIVE_TOPOLOGY topology) const;
    std::string FormatText(const BYTE* data, DXGI_FORMAT format) const;

    void ReleaseFileResources();
    void ReleaseCaptures();

    ID3D12Device* m_device = nullptr;
    mutable std::mutex m_mutex;
    std::unordered_map<ID3D12PipelineState*, PipelineInfo> m_pipelines;
    std::unordered_map<ID3D12Resource*, ResourceInfo> m_resources;
    std::unordered_map<ID3D12GraphicsCommandList*, DrawState> m_lists;
    std::unordered_map<UINT64, bool> m_skipShaders;
    std::vector<TextureOverride> m_textureOverrides;
    std::unordered_map<std::wstring, ResourceFile> m_fileResources;
    std::vector<Capture> m_captures;

    std::wstring m_analysisDir;
    std::ofstream m_log;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_frameAnalysis{false};
    bool m_captureVB = true;
    bool m_captureIB = true;
    bool m_captureOnce = false;
    bool m_f8Latch = false;
    bool m_autoFrameAnalysis = false;
    bool m_endAnalysisOnPresent = true;
    UINT m_drawCall = 0;
    UINT m_frameNumber = 1;
};

extern "C" DX12Runtime* GetDX12Runtime();
