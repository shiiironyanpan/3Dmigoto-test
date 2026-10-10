#pragma once
#include "fakewin.h"
template <class T> const GUID& mig_uuidof() { static GUID g = {}; return g; }
#define __uuidof(T) (mig_uuidof<T>())
struct IUnknown { virtual HRESULT QueryInterface(REFIID, void**) = 0; virtual ULONG AddRef() = 0; virtual ULONG Release() = 0; };
typedef int D3D_FEATURE_LEVEL; typedef int D3D12_PRIMITIVE_TOPOLOGY;
enum DXGI_FORMAT { DXGI_FORMAT_UNKNOWN = 0 };
struct DXGI_SAMPLE_DESC { UINT Count, Quality; };
enum D3D12_HEAP_TYPE { D3D12_HEAP_TYPE_DEFAULT = 1, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_TYPE_CUSTOM };
enum D3D12_CPU_PAGE_PROPERTY { D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK };
enum D3D12_HEAP_FLAGS { D3D12_HEAP_FLAG_NONE = 0 };
enum D3D12_RESOURCE_DIMENSION { D3D12_RESOURCE_DIMENSION_UNKNOWN, D3D12_RESOURCE_DIMENSION_BUFFER };
enum D3D12_RESOURCE_STATES { D3D12_RESOURCE_STATE_GENERIC_READ = 0xAC3 };
enum D3D12_TEXTURE_LAYOUT { D3D12_TEXTURE_LAYOUT_ROW_MAJOR = 1 };
enum D3D12_RESOURCE_FLAGS { D3D12_RESOURCE_FLAG_NONE = 0 };
enum D3D12_COMMAND_LIST_TYPE { D3D12_COMMAND_LIST_TYPE_DIRECT };
enum D3D12_PRIMITIVE_TOPOLOGY_TYPE { D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH };
enum D3D12_INPUT_CLASSIFICATION { D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA };
enum D3D12_PIPELINE_STATE_SUBOBJECT_TYPE { D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX = 100 };
enum D3D12_INDEX_BUFFER_STRIP_CUT_VALUE { D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED };
enum D3D12_PIPELINE_STATE_FLAGS { D3D12_PIPELINE_STATE_FLAG_NONE };
struct D3D12_HEAP_PROPERTIES { D3D12_HEAP_TYPE Type; D3D12_CPU_PAGE_PROPERTY CPUPageProperty; int MemoryPoolPreference; UINT CreationNodeMask, VisibleNodeMask; };
struct D3D12_RESOURCE_DESC { D3D12_RESOURCE_DIMENSION Dimension; UINT64 Alignment, Width; UINT Height; WORD DepthOrArraySize, MipLevels; DXGI_FORMAT Format; DXGI_SAMPLE_DESC SampleDesc; D3D12_TEXTURE_LAYOUT Layout; D3D12_RESOURCE_FLAGS Flags; };
struct D3D12_HEAP_DESC { UINT64 SizeInBytes; D3D12_HEAP_PROPERTIES Properties; UINT64 Alignment; int Flags; };
struct D3D12_CLEAR_VALUE { int x; };
struct D3D12_RANGE { SIZE_T Begin, End; };
struct D3D12_SHADER_BYTECODE { const void* pShaderBytecode; SIZE_T BytecodeLength; };
struct D3D12_INPUT_ELEMENT_DESC { const char* SemanticName; UINT SemanticIndex; DXGI_FORMAT Format; UINT InputSlot, AlignedByteOffset; D3D12_INPUT_CLASSIFICATION InputSlotClass; UINT InstanceDataStepRate; };
struct D3D12_INPUT_LAYOUT_DESC { const D3D12_INPUT_ELEMENT_DESC* pInputElementDescs; UINT NumElements; };
struct D3D12_CACHED_PIPELINE_STATE { const void* pCachedBlob; SIZE_T CachedBlobSizeInBytes; };
struct D3D12_STREAM_OUTPUT_DESC { const void* a; UINT b; const UINT* c; UINT d, e; };
struct D3D12_BLEND_DESC { BOOL a, b; UINT c[8][4]; };
struct D3D12_RASTERIZER_DESC { int a[3]; float f[3]; };
struct D3D12_DEPTH_STENCIL_DESC { int a[10]; };
struct D3D12_DEPTH_STENCIL_DESC1 { int a[12]; };
struct D3D12_RT_FORMAT_ARRAY { DXGI_FORMAT RTFormats[8]; UINT NumRenderTargets; };
struct D3D12_VIEW_INSTANCING_DESC { UINT n; const void* p; int f; };
class ID3D12RootSignature;
struct D3D12_GRAPHICS_PIPELINE_STATE_DESC { ID3D12RootSignature* pRootSignature; D3D12_SHADER_BYTECODE VS, PS, DS, HS, GS; D3D12_STREAM_OUTPUT_DESC StreamOutput; D3D12_BLEND_DESC BlendState; UINT SampleMask; D3D12_RASTERIZER_DESC RasterizerState; D3D12_DEPTH_STENCIL_DESC DepthStencilState; D3D12_INPUT_LAYOUT_DESC InputLayout; D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue; D3D12_PRIMITIVE_TOPOLOGY_TYPE PrimitiveTopologyType; UINT NumRenderTargets; DXGI_FORMAT RTVFormats[8]; DXGI_FORMAT DSVFormat; DXGI_SAMPLE_DESC SampleDesc; UINT NodeMask; D3D12_CACHED_PIPELINE_STATE CachedPSO; D3D12_PIPELINE_STATE_FLAGS Flags; };
struct D3D12_COMPUTE_PIPELINE_STATE_DESC { ID3D12RootSignature* pRootSignature; D3D12_SHADER_BYTECODE CS; UINT NodeMask; D3D12_CACHED_PIPELINE_STATE CachedPSO; D3D12_PIPELINE_STATE_FLAGS Flags; };
struct D3D12_PIPELINE_STATE_STREAM_DESC { SIZE_T SizeInBytes; void* pPipelineStateSubobjectStream; };
typedef UINT64 D3D12_GPU_VIRTUAL_ADDRESS;
struct D3D12_INDEX_BUFFER_VIEW { D3D12_GPU_VIRTUAL_ADDRESS BufferLocation; UINT SizeInBytes; DXGI_FORMAT Format; };
struct D3D12_VERTEX_BUFFER_VIEW { D3D12_GPU_VIRTUAL_ADDRESS BufferLocation; UINT SizeInBytes, StrideInBytes; };
struct ID3D12Object : IUnknown {};
struct ID3D12PipelineState : ID3D12Object {};
struct ID3D12RootSignature : ID3D12Object {};
struct ID3D12CommandAllocator : ID3D12Object {};
struct ID3D12Heap : ID3D12Object { virtual D3D12_HEAP_DESC GetDesc() = 0; };
struct ID3D12Resource : ID3D12Object {
	virtual HRESULT Map(UINT, const D3D12_RANGE*, void**) = 0; virtual void Unmap(UINT, const D3D12_RANGE*) = 0;
	virtual D3D12_RESOURCE_DESC GetDesc() = 0; virtual D3D12_GPU_VIRTUAL_ADDRESS GetGPUVirtualAddress() = 0; };
struct ID3D12GraphicsCommandList : ID3D12Object {
	virtual HRESULT Reset(ID3D12CommandAllocator*, ID3D12PipelineState*) = 0; virtual void ClearState(ID3D12PipelineState*) = 0;
	virtual void DrawInstanced(UINT, UINT, UINT, UINT) = 0; virtual void DrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) = 0;
	virtual void Dispatch(UINT, UINT, UINT) = 0; virtual void CopyBufferRegion(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64) = 0;
	virtual void CopyResource(ID3D12Resource*, ID3D12Resource*) = 0; virtual void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY) = 0;
	virtual void SetPipelineState(ID3D12PipelineState*) = 0; virtual void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW*) = 0;
	virtual void IASetVertexBuffers(UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*) = 0; };
struct ID3D12Device : ID3D12Object {
	virtual HRESULT CreateCommandList(UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**) = 0;
	virtual HRESULT CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**) = 0;
	virtual HRESULT CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**) = 0;
	virtual HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) = 0;
	virtual HRESULT CreatePlacedResource(ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) = 0;
	virtual HRESULT CreateReservedResource(const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**) = 0; };
struct ID3D12Device2 : ID3D12Device { virtual HRESULT CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**) = 0; };
struct ID3D12Device4 : ID3D12Device2 { virtual HRESULT CreateCommandList1(UINT, D3D12_COMMAND_LIST_TYPE, UINT, REFIID, void**) = 0;
	virtual HRESULT CreateCommittedResource1(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, void*, REFIID, void**) = 0; };
// ---- DXGI
#define DXGI_PRESENT_TEST 0x1
typedef const GUID& REFGUID;
inline bool operator==(const GUID& a, const GUID& b) { return !memcmp(&a, &b, sizeof(GUID)); }
#define S_OK ((HRESULT)0)
#define E_POINTER ((HRESULT)0x80004003)
#define E_NOINTERFACE ((HRESULT)0x80004002)
struct DXGI_RATIONAL { UINT n, d; };
struct DXGI_MODE_DESC { UINT Width, Height; DXGI_RATIONAL r; DXGI_FORMAT Format; };
struct DXGI_SWAP_CHAIN_DESC { DXGI_MODE_DESC BufferDesc; UINT BufferCount; HWND OutputWindow; };
struct DXGI_SWAP_CHAIN_DESC1 { UINT Width; }; struct DXGI_SWAP_CHAIN_FULLSCREEN_DESC { UINT a; }; struct DXGI_PRESENT_PARAMETERS { UINT a; };
struct DXGI_FRAME_STATISTICS { UINT a; }; struct DXGI_RGBA { float r, g, b, a; }; struct DXGI_MATRIX_3X2_F { float m[6]; };
enum DXGI_MODE_ROTATION { DXGI_MODE_ROTATION_UNSPECIFIED }; enum DXGI_COLOR_SPACE_TYPE { DXGI_COLOR_SPACE_RESERVED }; enum DXGI_HDR_METADATA_TYPE { DXGI_HDR_METADATA_TYPE_NONE };
struct IDXGIOutput : IUnknown {};
struct IDXGIObject : IUnknown {
	virtual HRESULT SetPrivateData(REFGUID, UINT, const void*) = 0; virtual HRESULT SetPrivateDataInterface(REFGUID, const IUnknown*) = 0;
	virtual HRESULT GetPrivateData(REFGUID, UINT*, void*) = 0; virtual HRESULT GetParent(REFIID, void**) = 0; };
struct IDXGIDeviceSubObject : IDXGIObject { virtual HRESULT GetDevice(REFIID, void**) = 0; };
struct IDXGISwapChain : IDXGIDeviceSubObject {
	virtual HRESULT Present(UINT, UINT) = 0; virtual HRESULT GetBuffer(UINT, REFIID, void**) = 0;
	virtual HRESULT SetFullscreenState(BOOL, IDXGIOutput*) = 0; virtual HRESULT GetFullscreenState(BOOL*, IDXGIOutput**) = 0;
	virtual HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC*) = 0; virtual HRESULT ResizeBuffers(UINT, UINT, UINT, DXGI_FORMAT, UINT) = 0;
	virtual HRESULT ResizeTarget(const DXGI_MODE_DESC*) = 0; virtual HRESULT GetContainingOutput(IDXGIOutput**) = 0;
	virtual HRESULT GetFrameStatistics(DXGI_FRAME_STATISTICS*) = 0; virtual HRESULT GetLastPresentCount(UINT*) = 0; };
struct IDXGISwapChain1 : IDXGISwapChain {
	virtual HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1*) = 0; virtual HRESULT GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC*) = 0;
	virtual HRESULT GetHwnd(HWND*) = 0; virtual HRESULT GetCoreWindow(REFIID, void**) = 0;
	virtual HRESULT Present1(UINT, UINT, const DXGI_PRESENT_PARAMETERS*) = 0; virtual BOOL IsTemporaryMonoSupported() = 0;
	virtual HRESULT GetRestrictToOutput(IDXGIOutput**) = 0; virtual HRESULT SetBackgroundColor(const DXGI_RGBA*) = 0;
	virtual HRESULT GetBackgroundColor(DXGI_RGBA*) = 0; virtual HRESULT SetRotation(DXGI_MODE_ROTATION) = 0; virtual HRESULT GetRotation(DXGI_MODE_ROTATION*) = 0; };
struct IDXGISwapChain2 : IDXGISwapChain1 {
	virtual HRESULT SetSourceSize(UINT, UINT) = 0; virtual HRESULT GetSourceSize(UINT*, UINT*) = 0;
	virtual HRESULT SetMaximumFrameLatency(UINT) = 0; virtual HRESULT GetMaximumFrameLatency(UINT*) = 0;
	virtual HANDLE GetFrameLatencyWaitableObject() = 0; virtual HRESULT SetMatrixTransform(const DXGI_MATRIX_3X2_F*) = 0; virtual HRESULT GetMatrixTransform(DXGI_MATRIX_3X2_F*) = 0; };
struct IDXGISwapChain3 : IDXGISwapChain2 {
	virtual UINT GetCurrentBackBufferIndex() = 0; virtual HRESULT CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE, UINT*) = 0;
	virtual HRESULT SetColorSpace1(DXGI_COLOR_SPACE_TYPE) = 0;
	virtual HRESULT ResizeBuffers1(UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*) = 0; };
struct IDXGISwapChain4 : IDXGISwapChain3 { virtual HRESULT SetHDRMetaData(DXGI_HDR_METADATA_TYPE, UINT, void*) = 0; };
struct IDXGIFactory : IDXGIObject { virtual HRESULT CreateSwapChain(IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**) = 0; };
struct IDXGIFactory1 : IDXGIFactory {};
struct IDXGIFactory2 : IDXGIFactory1 { virtual HRESULT CreateSwapChainForHwnd(IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**) = 0; };
struct ID3D12CommandQueue : ID3D12Object {};
