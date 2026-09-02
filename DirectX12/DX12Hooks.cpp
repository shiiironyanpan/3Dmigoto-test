#include "DX12Hooks.h"
#include "DX12Runtime.h"
#include "../ThirdPartyLibs/Nektra-lib/NktHookLib.h"
#include "../log.h"

#include <mutex>
#include <vector>

CNktHookLib g_dx12Hooks;
thread_local bool g_dx12Internal = false;

namespace {

using PFN_CreateCommandQueue = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using PFN_CreateGraphicsPipelineState = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateComputePipelineState = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using PFN_CreateCommandList = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using PFN_CreateCommandList1 = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
using PFN_CreateCommittedResource = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreatePlacedResource = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateReservedResource = HRESULT (STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_ExecuteCommandLists = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_ExecuteIndirect = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using PFN_Reset = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_Close = HRESULT (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_SetPipelineState = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_ResourceBarrier = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
using PFN_IASetIndexBuffer = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using PFN_IASetVertexBuffers = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using PFN_IASetPrimitiveTopology = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
using PFN_DrawIndexedInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_Barrier = void (STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT32, const D3D12_BARRIER_GROUP*);

using PFN_CreateDXGIFactory = HRESULT (WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory1 = HRESULT (WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT (WINAPI*)(UINT, REFIID, void**);
using PFN_FactoryCreateSwapChain = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_FactoryCreateSwapChainForHwnd = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_FactoryCreateSwapChainForCoreWindow = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_FactoryCreateSwapChainForComposition = HRESULT (STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using PFN_Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

PFN_CreateCommandQueue oCreateCommandQueue = nullptr;
PFN_CreateGraphicsPipelineState oCreateGraphicsPipelineState = nullptr;
PFN_CreateComputePipelineState oCreateComputePipelineState = nullptr;
PFN_CreateCommandList oCreateCommandList = nullptr;
PFN_CreateCommandList1 oCreateCommandList1 = nullptr;
PFN_CreateCommittedResource oCreateCommittedResource = nullptr;
PFN_CreatePlacedResource oCreatePlacedResource = nullptr;
PFN_CreateReservedResource oCreateReservedResource = nullptr;
PFN_ExecuteCommandLists oExecuteCommandLists = nullptr;
PFN_Reset oReset = nullptr;
PFN_Close oClose = nullptr;
PFN_SetPipelineState oSetPipelineState = nullptr;
PFN_ResourceBarrier oResourceBarrier = nullptr;
PFN_IASetIndexBuffer oIASetIndexBuffer = nullptr;
PFN_IASetVertexBuffers oIASetVertexBuffers = nullptr;
PFN_IASetPrimitiveTopology oIASetPrimitiveTopology = nullptr;
PFN_DrawIndexedInstanced oDrawIndexedInstanced = nullptr;
PFN_DrawInstanced oDrawInstanced = nullptr;
PFN_ExecuteIndirect oExecuteIndirect = nullptr;
PFN_Barrier oBarrier = nullptr;

PFN_CreateDXGIFactory oCreateDXGIFactory = nullptr;
PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = nullptr;
PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = nullptr;
PFN_FactoryCreateSwapChain oFactoryCreateSwapChain = nullptr;
PFN_FactoryCreateSwapChainForHwnd oFactoryCreateSwapChainForHwnd = nullptr;
PFN_FactoryCreateSwapChainForCoreWindow oFactoryCreateSwapChainForCoreWindow = nullptr;
PFN_FactoryCreateSwapChainForComposition oFactoryCreateSwapChainForComposition = nullptr;
PFN_Present oPresent = nullptr;
PFN_Present1 oPresent1 = nullptr;

std::once_flag g_deviceHooksOnce;
std::once_flag g_queueHooksOnce;
std::once_flag g_listHooksOnce;
std::once_flag g_factoryHooksOnce;
std::once_flag g_swapChainHooksOnce;
std::once_flag g_runtimeEntryHooksOnce;

void HookOne(void* target, void** original, void* detour, const char* name)
{
    if (!target) return;
    SIZE_T id = 0;
    DWORD err = g_dx12Hooks.Hook(&id, original, target, detour);
    if (err != ERROR_SUCCESS)
        LogInfo("DX12: failed hook %s: %lu\n", name, err);
}

static HRESULT STDMETHODCALLTYPE hkCreateCommandQueue(ID3D12Device* d, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID riid, void** out)
{
    HRESULT hr = oCreateCommandQueue(d, desc, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12CommandQueue* q = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&q)))) {
            DX12InstallQueueHooks(q);
            q->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateGraphicsPipelineState(ID3D12Device* d, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
    if (!desc) return oCreateGraphicsPipelineState(d, desc, riid, out);

    DX12Runtime* rt = GetDX12Runtime();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC modified = *desc;
    std::vector<BYTE> replacements[5];
    const D3D12_SHADER_BYTECODE* stages[5] = { &desc->VS, &desc->HS, &desc->DS, &desc->GS, &desc->PS };
    D3D12_SHADER_BYTECODE* modifiedStages[5] = { &modified.VS, &modified.HS, &modified.DS, &modified.GS, &modified.PS };
    const char* names[5] = { "vs", "hs", "ds", "gs", "ps" };

    for (int i = 0; i < 5; ++i) {
        if (!stages[i]->pShaderBytecode || !stages[i]->BytecodeLength) continue;
        UINT64 hash = rt->HashShader(stages[i]->pShaderBytecode, stages[i]->BytecodeLength);
        if (rt->LoadShaderReplacement(hash, names[i], replacements[i]))
            *modifiedStages[i] = { replacements[i].data(), replacements[i].size() };
    }

    UINT64 originalHash = rt->HashPipeline(desc);
    HRESULT hr = oCreateGraphicsPipelineState(d, &modified, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&pso)))) {
            rt->OnGraphicsPipelineCreated(desc, pso, originalHash);
            pso->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateComputePipelineState(ID3D12Device* d, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
    if (!desc) return oCreateComputePipelineState(d, desc, riid, out);
    DX12Runtime* rt = GetDX12Runtime();
    D3D12_COMPUTE_PIPELINE_STATE_DESC modified = *desc;
    std::vector<BYTE> cs;
    if (desc->CS.pShaderBytecode && desc->CS.BytecodeLength) {
        UINT64 hash = rt->HashShader(desc->CS.pShaderBytecode, desc->CS.BytecodeLength);
        if (rt->LoadShaderReplacement(hash, "cs", cs))
            modified.CS = { cs.data(), cs.size() };
    }
    HRESULT hr = oCreateComputePipelineState(d, &modified, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&pso)))) {
            rt->OnComputePipelineCreated(desc, pso, rt->HashShader(desc->CS.pShaderBytecode, desc->CS.BytecodeLength));
            pso->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateCommandList(ID3D12Device* d, UINT node, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* a, ID3D12PipelineState* p, REFIID riid, void** out)
{
    HRESULT hr = oCreateCommandList(d, node, type, a, p, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&list)))) {
            DX12InstallCommandListHooks(list);
            GetDX12Runtime()->TrackSetPipelineState(list, p);
            list->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateCommandList1(ID3D12Device* d, UINT node, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** out)
{
    HRESULT hr = oCreateCommandList1(d, node, type, flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&list)))) {
            DX12InstallCommandListHooks(list);
            list->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateCommittedResource(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
    HRESULT hr = oCreateCommittedResource(d, hp, hf, desc, state, clear, riid, out);
    if (!g_dx12Internal && SUCCEEDED(hr) && out && *out) {
        ID3D12Resource* resource = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&resource)))) {
            GetDX12Runtime()->RegisterResource(resource, state);
            resource->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreatePlacedResource(ID3D12Device* d, ID3D12Heap* heap, UINT64 offset, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
    HRESULT hr = oCreatePlacedResource(d, heap, offset, desc, state, clear, riid, out);
    if (!g_dx12Internal && SUCCEEDED(hr) && out && *out) {
        ID3D12Resource* resource = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&resource)))) {
            GetDX12Runtime()->RegisterResource(resource, state);
            resource->Release();
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateReservedResource(ID3D12Device* d, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
    HRESULT hr = oCreateReservedResource(d, desc, state, clear, riid, out);
    if (!g_dx12Internal && SUCCEEDED(hr) && out && *out) {
        ID3D12Resource* resource = nullptr;
        if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&resource)))) {
            GetDX12Runtime()->RegisterResource(resource, state);
            resource->Release();
        }
    }
    return hr;
}

static void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists)
{
    oExecuteCommandLists(q, count, lists);
    if (!g_dx12Internal) GetDX12Runtime()->OnExecute(q, count, lists);
}

static HRESULT STDMETHODCALLTYPE hkReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso)
{
    HRESULT hr = oReset(list, allocator, pso);
    if (SUCCEEDED(hr)) GetDX12Runtime()->ResetCommandList(list, pso);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkClose(ID3D12GraphicsCommandList* list)
{
    HRESULT hr = oClose(list);
    if (SUCCEEDED(hr)) GetDX12Runtime()->OnCommandListClose(list);
    return hr;
}

static void STDMETHODCALLTYPE hkSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
    oSetPipelineState(list, pso);
    GetDX12Runtime()->TrackSetPipelineState(list, pso);
}

static void STDMETHODCALLTYPE hkResourceBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
{
    oResourceBarrier(list, count, barriers);
    if (!g_dx12Internal) GetDX12Runtime()->OnResourceBarrier(count, barriers);
}

static void STDMETHODCALLTYPE hkBarrier(ID3D12GraphicsCommandList* list, UINT numGroups, const D3D12_BARRIER_GROUP* groups)
{
    oBarrier(list, numGroups, groups);
    if (!g_dx12Internal) GetDX12Runtime()->OnEnhancedBarrier(numGroups, groups);
}

static void STDMETHODCALLTYPE hkIASetIndexBuffer(ID3D12GraphicsCommandList* list, const D3D12_INDEX_BUFFER_VIEW* view)
{
    oIASetIndexBuffer(list, view);
    GetDX12Runtime()->TrackIAIndex(list, view);
}

static void STDMETHODCALLTYPE hkIASetVertexBuffers(ID3D12GraphicsCommandList* list, UINT startSlot, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views)
{
    std::vector<D3D12_VERTEX_BUFFER_VIEW> modified;
    if (views && count) {
        modified.assign(views, views + count);
        GetDX12Runtime()->ApplyVertexOverrides(list, startSlot, count, modified.data());
        oIASetVertexBuffers(list, startSlot, count, modified.data());
        // Track the application's binding, not our replacement binding. This keeps Frame
        // Analysis faithful to the original draw while the replacement is only used by GPU execution.
        GetDX12Runtime()->TrackIAVertex(list, startSlot, count, views);
    } else {
        oIASetVertexBuffers(list, startSlot, count, views);
        GetDX12Runtime()->TrackIAVertex(list, startSlot, count, views);
    }
}

static void STDMETHODCALLTYPE hkIASetPrimitiveTopology(ID3D12GraphicsCommandList* list, D3D12_PRIMITIVE_TOPOLOGY topology)
{
    oIASetPrimitiveTopology(list, topology);
    GetDX12Runtime()->TrackIATopology(list, topology);
}

static void STDMETHODCALLTYPE hkDrawIndexedInstanced(ID3D12GraphicsCommandList* list, UINT indexCount, UINT instanceCount, UINT firstIndex, INT baseVertex, UINT firstInstance)
{
    DX12Runtime* rt = GetDX12Runtime();
    bool skip = rt->ShouldSkipDraw(list, true, firstIndex, indexCount, firstInstance, instanceCount);
    if (!skip) {
        UINT actualCount = indexCount, actualFirst = firstIndex;
        INT actualBase = baseVertex;
        rt->GetDrawOverride(list, true, &actualCount, &actualFirst, &actualBase);
        D3D12_INDEX_BUFFER_VIEW overrideIB{};
        if (rt->ApplyIndexOverrideForDraw(list, firstIndex, indexCount, baseVertex, firstInstance, instanceCount, &overrideIB)) {
            oIASetIndexBuffer(list, &overrideIB);
        }
        rt->OnDrawIndexed(indexCount, instanceCount, firstIndex, baseVertex, firstInstance, list);
        oDrawIndexedInstanced(list, actualCount, instanceCount, actualFirst, actualBase, firstInstance);
    }
}

static void STDMETHODCALLTYPE hkDrawInstanced(ID3D12GraphicsCommandList* list, UINT vertexCount, UINT instanceCount, UINT firstVertex, UINT firstInstance)
{
    DX12Runtime* rt = GetDX12Runtime();
    bool skip = rt->ShouldSkipDraw(list, false, firstVertex, vertexCount, firstInstance, instanceCount);
    if (!skip) {
        UINT actualCount = vertexCount, actualFirst = firstVertex;
        INT dummyBase = 0;
        rt->GetDrawOverride(list, false, &actualCount, &actualFirst, &dummyBase);
        rt->OnDraw(vertexCount, instanceCount, firstVertex, firstInstance, list);
        oDrawInstanced(list, actualCount, instanceCount, actualFirst, firstInstance);
    }
}

static void STDMETHODCALLTYPE hkExecuteIndirect(ID3D12GraphicsCommandList* list, ID3D12CommandSignature* signature, UINT maxCommandCount, ID3D12Resource* argumentBuffer, UINT64 argumentBufferOffset, ID3D12Resource* countBuffer, UINT64 countBufferOffset)
{
    // The argument packet is GPU data for many engines. We preserve the native call and
    // record the currently bound mesh state so the resource dumps remain available.
    oExecuteIndirect(list, signature, maxCommandCount, argumentBuffer, argumentBufferOffset, countBuffer, countBufferOffset);
}

static HRESULT STDMETHODCALLTYPE hkFactoryCreateSwapChain(IDXGIFactory* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** out)
{
    HRESULT hr = oFactoryCreateSwapChain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) DX12InstallSwapChainHooks(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkFactoryCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* output, IDXGISwapChain1** out)
{
    HRESULT hr = oFactoryCreateSwapChainForHwnd(factory, device, hwnd, desc, fs, output, out);
    if (SUCCEEDED(hr) && out && *out) DX12InstallSwapChainHooks(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkFactoryCreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out)
{
    HRESULT hr = oFactoryCreateSwapChainForCoreWindow(factory, device, window, desc, output, out);
    if (SUCCEEDED(hr) && out && *out) DX12InstallSwapChainHooks(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkFactoryCreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output, IDXGISwapChain1** out)
{
    HRESULT hr = oFactoryCreateSwapChainForComposition(factory, device, desc, output, out);
    if (SUCCEEDED(hr) && out && *out) DX12InstallSwapChainHooks(*out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swapChain, UINT sync, UINT flags)
{
    HRESULT hr = oPresent(swapChain, sync, flags);
    if (SUCCEEDED(hr) && !g_dx12Internal) GetDX12Runtime()->OnPresent();
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* swapChain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
{
    HRESULT hr = oPresent1(swapChain, sync, flags, params);
    if (SUCCEEDED(hr) && !g_dx12Internal) GetDX12Runtime()->OnPresent();
    return hr;
}

static HRESULT WINAPI hkCreateDXGIFactory(REFIID riid, void** out)
{
    HRESULT hr = oCreateDXGIFactory(riid, out);
    if (SUCCEEDED(hr) && out && *out) DX12InstallFactoryHooks(static_cast<IUnknown*>(*out));
    return hr;
}

static HRESULT WINAPI hkCreateDXGIFactory1(REFIID riid, void** out)
{
    HRESULT hr = oCreateDXGIFactory1(riid, out);
    if (SUCCEEDED(hr) && out && *out) DX12InstallFactoryHooks(static_cast<IUnknown*>(*out));
    return hr;
}

static HRESULT WINAPI hkCreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    HRESULT hr = oCreateDXGIFactory2(flags, riid, out);
    if (SUCCEEDED(hr) && out && *out) DX12InstallFactoryHooks(static_cast<IUnknown*>(*out));
    return hr;
}

} // namespace

void DX12InstallDeviceHooks(ID3D12Device* device)
{
    if (!device) return;
    std::call_once(g_deviceHooksOnce, [device]() {
        void** v = *reinterpret_cast<void***>(device);
        HookOne(v[8], reinterpret_cast<void**>(&oCreateCommandQueue), reinterpret_cast<void*>(hkCreateCommandQueue), "ID3D12Device::CreateCommandQueue");
        HookOne(v[10], reinterpret_cast<void**>(&oCreateGraphicsPipelineState), reinterpret_cast<void*>(hkCreateGraphicsPipelineState), "ID3D12Device::CreateGraphicsPipelineState");
        HookOne(v[11], reinterpret_cast<void**>(&oCreateComputePipelineState), reinterpret_cast<void*>(hkCreateComputePipelineState), "ID3D12Device::CreateComputePipelineState");
        HookOne(v[12], reinterpret_cast<void**>(&oCreateCommandList), reinterpret_cast<void*>(hkCreateCommandList), "ID3D12Device::CreateCommandList");
        ID3D12Device4* device4 = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)))) {
            void** v4 = *reinterpret_cast<void***>(device4);
            HookOne(v4[51], reinterpret_cast<void**>(&oCreateCommandList1), reinterpret_cast<void*>(hkCreateCommandList1), "ID3D12Device4::CreateCommandList1");
            device4->Release();
        }
        HookOne(v[27], reinterpret_cast<void**>(&oCreateCommittedResource), reinterpret_cast<void*>(hkCreateCommittedResource), "ID3D12Device::CreateCommittedResource");
        HookOne(v[29], reinterpret_cast<void**>(&oCreatePlacedResource), reinterpret_cast<void*>(hkCreatePlacedResource), "ID3D12Device::CreatePlacedResource");
        HookOne(v[30], reinterpret_cast<void**>(&oCreateReservedResource), reinterpret_cast<void*>(hkCreateReservedResource), "ID3D12Device::CreateReservedResource");
    });
}

void DX12InstallQueueHooks(ID3D12CommandQueue* queue)
{
    if (!queue) return;
    std::call_once(g_queueHooksOnce, [queue]() {
        void** v = *reinterpret_cast<void***>(queue);
        HookOne(v[10], reinterpret_cast<void**>(&oExecuteCommandLists), reinterpret_cast<void*>(hkExecuteCommandLists), "ID3D12CommandQueue::ExecuteCommandLists");
    });
}

void DX12InstallCommandListHooks(ID3D12GraphicsCommandList* list)
{
    if (!list) return;
    std::call_once(g_listHooksOnce, [list]() {
        void** v = *reinterpret_cast<void***>(list);
        HookOne(v[10], reinterpret_cast<void**>(&oReset), reinterpret_cast<void*>(hkReset), "ID3D12GraphicsCommandList::Reset");
        HookOne(v[9], reinterpret_cast<void**>(&oClose), reinterpret_cast<void*>(hkClose), "ID3D12GraphicsCommandList::Close");
        HookOne(v[12], reinterpret_cast<void**>(&oDrawInstanced), reinterpret_cast<void*>(hkDrawInstanced), "ID3D12GraphicsCommandList::DrawInstanced");
        HookOne(v[13], reinterpret_cast<void**>(&oDrawIndexedInstanced), reinterpret_cast<void*>(hkDrawIndexedInstanced), "ID3D12GraphicsCommandList::DrawIndexedInstanced");
        HookOne(v[20], reinterpret_cast<void**>(&oIASetPrimitiveTopology), reinterpret_cast<void*>(hkIASetPrimitiveTopology), "ID3D12GraphicsCommandList::IASetPrimitiveTopology");
        HookOne(v[25], reinterpret_cast<void**>(&oSetPipelineState), reinterpret_cast<void*>(hkSetPipelineState), "ID3D12GraphicsCommandList::SetPipelineState");
        HookOne(v[26], reinterpret_cast<void**>(&oResourceBarrier), reinterpret_cast<void*>(hkResourceBarrier), "ID3D12GraphicsCommandList::ResourceBarrier");
        HookOne(v[43], reinterpret_cast<void**>(&oIASetIndexBuffer), reinterpret_cast<void*>(hkIASetIndexBuffer), "ID3D12GraphicsCommandList::IASetIndexBuffer");
        HookOne(v[44], reinterpret_cast<void**>(&oIASetVertexBuffers), reinterpret_cast<void*>(hkIASetVertexBuffers), "ID3D12GraphicsCommandList::IASetVertexBuffers");
        HookOne(v[59], reinterpret_cast<void**>(&oExecuteIndirect), reinterpret_cast<void*>(hkExecuteIndirect), "ID3D12GraphicsCommandList::ExecuteIndirect");
        ID3D12GraphicsCommandList7* list7 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list7)))) {
            void** v7 = *reinterpret_cast<void***>(list7);
            HookOne(v7[60], reinterpret_cast<void**>(&oBarrier), reinterpret_cast<void*>(hkBarrier), "ID3D12GraphicsCommandList7::Barrier");
            list7->Release();
        }
    });
}

void DX12InstallFactoryHooks(IUnknown* unknown)
{
    if (!unknown) return;
    std::call_once(g_factoryHooksOnce, [unknown]() {
        IDXGIFactory* factory = nullptr;
        if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&factory)))) return;
        void** v = *reinterpret_cast<void***>(factory);
        HookOne(v[10], reinterpret_cast<void**>(&oFactoryCreateSwapChain), reinterpret_cast<void*>(hkFactoryCreateSwapChain), "IDXGIFactory::CreateSwapChain");
        factory->Release();

        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&factory2)))) {
            void** v2 = *reinterpret_cast<void***>(factory2);
            HookOne(v2[15], reinterpret_cast<void**>(&oFactoryCreateSwapChainForHwnd), reinterpret_cast<void*>(hkFactoryCreateSwapChainForHwnd), "IDXGIFactory2::CreateSwapChainForHwnd");
            HookOne(v2[16], reinterpret_cast<void**>(&oFactoryCreateSwapChainForCoreWindow), reinterpret_cast<void*>(hkFactoryCreateSwapChainForCoreWindow), "IDXGIFactory2::CreateSwapChainForCoreWindow");
            HookOne(v2[24], reinterpret_cast<void**>(&oFactoryCreateSwapChainForComposition), reinterpret_cast<void*>(hkFactoryCreateSwapChainForComposition), "IDXGIFactory2::CreateSwapChainForComposition");
            factory2->Release();
        }
    });
}

void DX12InstallSwapChainHooks(IDXGISwapChain* swapChain)
{
    if (!swapChain) return;
    std::call_once(g_swapChainHooksOnce, [swapChain]() {
        void** v = *reinterpret_cast<void***>(swapChain);
        HookOne(v[8], reinterpret_cast<void**>(&oPresent), reinterpret_cast<void*>(hkPresent), "IDXGISwapChain::Present");
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            void** v1 = *reinterpret_cast<void***>(sc1);
            HookOne(v1[22], reinterpret_cast<void**>(&oPresent1), reinterpret_cast<void*>(hkPresent1), "IDXGISwapChain1::Present1");
            sc1->Release();
        }
    });
}

void DX12InstallRuntimeEntryHooks()
{
    std::call_once(g_runtimeEntryHooksOnce, []() {
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        if (!dxgi) dxgi = LoadLibraryW(L"dxgi.dll");
        if (!dxgi) return;
        FARPROC create0 = GetProcAddress(dxgi, "CreateDXGIFactory");
        FARPROC create1 = GetProcAddress(dxgi, "CreateDXGIFactory1");
        FARPROC create2 = GetProcAddress(dxgi, "CreateDXGIFactory2");
        HookOne(create0, reinterpret_cast<void**>(&oCreateDXGIFactory), reinterpret_cast<void*>(hkCreateDXGIFactory), "CreateDXGIFactory");
        HookOne(create1, reinterpret_cast<void**>(&oCreateDXGIFactory1), reinterpret_cast<void*>(hkCreateDXGIFactory1), "CreateDXGIFactory1");
        HookOne(create2, reinterpret_cast<void**>(&oCreateDXGIFactory2), reinterpret_cast<void*>(hkCreateDXGIFactory2), "CreateDXGIFactory2");

        // The game may have created its factory before D3D12CreateDevice. Create one ourselves
        // after installing the export hooks so the shared factory vtable is patched for already
        // existing factory objects as well.
        if (create1) {
            using CreateFactory1Fn = HRESULT (WINAPI*)(REFIID, void**);
            IDXGIFactory4* factory = nullptr;
            if (SUCCEEDED(reinterpret_cast<CreateFactory1Fn>(create1)(IID_PPV_ARGS(&factory)))) {
                DX12InstallFactoryHooks(factory);
                factory->Release();
            }
        }
    });
}
