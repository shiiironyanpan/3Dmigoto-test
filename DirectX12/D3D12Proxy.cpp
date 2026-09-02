#include <windows.h>

// d3d12.h declares these API entry points with DLL-import linkage. This file implements
// proxy exports with the same public names, so hide the imported declarations while the
// header is parsed to avoid C2375 "redefinition; different linkage" errors.
#define D3D12CreateDevice D3D12CreateDevice_Imported
#define D3D12GetDebugInterface D3D12GetDebugInterface_Imported
#define D3D12SerializeRootSignature D3D12SerializeRootSignature_Imported
#define D3D12SerializeVersionedRootSignature D3D12SerializeVersionedRootSignature_Imported
#define D3D12CreateRootSignatureDeserializer D3D12CreateRootSignatureDeserializer_Imported
#define D3D12CreateVersionedRootSignatureDeserializer D3D12CreateVersionedRootSignatureDeserializer_Imported
#define D3D12EnableExperimentalFeatures D3D12EnableExperimentalFeatures_Imported
#define D3D12GetInterface D3D12GetInterface_Imported
#include <d3d12.h>
#undef D3D12CreateDevice
#undef D3D12GetDebugInterface
#undef D3D12SerializeRootSignature
#undef D3D12SerializeVersionedRootSignature
#undef D3D12CreateRootSignatureDeserializer
#undef D3D12CreateVersionedRootSignatureDeserializer
#undef D3D12EnableExperimentalFeatures
#undef D3D12GetInterface

#include <string>
#include "DX12Runtime.h"
#include "DX12Hooks.h"
#include "../log.h"

static HMODULE g_realD3D12 = nullptr;
static FARPROC g_realCreateDevice = nullptr;
static FARPROC g_realGetDebugInterface = nullptr;
static FARPROC g_realSerializeRootSignature = nullptr;
static FARPROC g_realSerializeVersionedRootSignature = nullptr;
static FARPROC g_realCreateRootSignatureDeserializer = nullptr;
static FARPROC g_realCreateVersionedRootSignatureDeserializer = nullptr;
static FARPROC g_realEnableExperimentalFeatures = nullptr;
static FARPROC g_realGetInterface = nullptr;

static std::wstring SystemD3D12Path()
{
    wchar_t windir[MAX_PATH]{};
    GetWindowsDirectoryW(windir, ARRAYSIZE(windir));
    std::wstring p = windir;
    p += L"\\System32\\d3d12.dll";
    return p;
}

static bool LoadRealD3D12()
{
    if (g_realD3D12) return true;
    g_realD3D12 = LoadLibraryW(SystemD3D12Path().c_str());
    if (!g_realD3D12) return false;
    g_realCreateDevice = GetProcAddress(g_realD3D12, "D3D12CreateDevice");
    g_realGetDebugInterface = GetProcAddress(g_realD3D12, "D3D12GetDebugInterface");
    g_realSerializeRootSignature = GetProcAddress(g_realD3D12, "D3D12SerializeRootSignature");
    g_realSerializeVersionedRootSignature = GetProcAddress(g_realD3D12, "D3D12SerializeVersionedRootSignature");
    g_realCreateRootSignatureDeserializer = GetProcAddress(g_realD3D12, "D3D12CreateRootSignatureDeserializer");
    g_realCreateVersionedRootSignatureDeserializer = GetProcAddress(g_realD3D12, "D3D12CreateVersionedRootSignatureDeserializer");
    g_realEnableExperimentalFeatures = GetProcAddress(g_realD3D12, "D3D12EnableExperimentalFeatures");
    g_realGetInterface = GetProcAddress(g_realD3D12, "D3D12GetInterface");
    return g_realCreateDevice != nullptr;
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void** ppDevice)
{
    if (!LoadRealD3D12()) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HRESULT hr = reinterpret_cast<Fn>(g_realCreateDevice)(adapter, minimumFeatureLevel, riid, ppDevice);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        ID3D12Device* device = nullptr;
        if (SUCCEEDED((*ppDevice)->QueryInterface(IID_PPV_ARGS(&device)))) {
            DX12Runtime::Instance().Initialize(device);
            DX12InstallDeviceHooks(device);
            device->Release();
        }
    }
    return hr;
}

#define FORWARD0(name, rettype, sig, args) \
extern "C" __declspec(dllexport) rettype WINAPI name sig { \
    if (!LoadRealD3D12() || !g_real##name) return (rettype)E_FAIL; \
    return reinterpret_cast<rettype (WINAPI*) sig>(g_real##name) args; }

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** ppvDebug)
{
    if (!LoadRealD3D12() || !g_realGetDebugInterface) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(REFIID, void**);
    return reinterpret_cast<Fn>(g_realGetDebugInterface)(riid, ppvDebug);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC* desc, D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob, ID3DBlob** error)
{
    if (!LoadRealD3D12() || !g_realSerializeRootSignature) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    return reinterpret_cast<Fn>(g_realSerializeRootSignature)(desc, version, blob, error);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12SerializeVersionedRootSignature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc, ID3DBlob** blob, ID3DBlob** error)
{
    if (!LoadRealD3D12() || !g_realSerializeVersionedRootSignature) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    return reinterpret_cast<Fn>(g_realSerializeVersionedRootSignature)(desc, blob, error);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12CreateRootSignatureDeserializer(LPCVOID src, SIZE_T size, REFIID riid, void** out)
{
    if (!LoadRealD3D12() || !g_realCreateRootSignatureDeserializer) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    return reinterpret_cast<Fn>(g_realCreateRootSignatureDeserializer)(src, size, riid, out);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(LPCVOID src, SIZE_T size, REFIID riid, void** out)
{
    if (!LoadRealD3D12() || !g_realCreateVersionedRootSignatureDeserializer) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    return reinterpret_cast<Fn>(g_realCreateVersionedRootSignatureDeserializer)(src, size, riid, out);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void** out)
{
    if (!LoadRealD3D12() || !g_realGetInterface) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
    return reinterpret_cast<Fn>(g_realGetInterface)(clsid, riid, out);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT count, const IID* features, void* config, UINT* configSize)
{
    if (!LoadRealD3D12() || !g_realEnableExperimentalFeatures) return E_FAIL;
    using Fn = HRESULT (WINAPI*)(UINT, const IID*, void*, UINT*);
    return reinterpret_cast<Fn>(g_realEnableExperimentalFeatures)(count, features, config, configSize);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadRealD3D12();
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            // Polling avoids relying on undocumented loader-notification APIs and lets us install
            // the DXGI factory hooks after dxgi.dll has been loaded by the game.
            for (int i = 0; i < 600; ++i) {
                if (GetModuleHandleW(L"dxgi.dll")) {
                    DX12InstallRuntimeEntryHooks();
                    break;
                }
                Sleep(10);
            }
            return 0;
        }, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        DX12Runtime::Instance().Shutdown();
        if (g_realD3D12) { FreeLibrary(g_realD3D12); g_realD3D12 = nullptr; }
    }
    return TRUE;
}
